// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CascadiaSettings.h"

#include <fmt/chrono.h>
#include <shlobj.h>
#include <til/latch.h>

// defaults.h is a file containing the default json settings in a std::string_view
#include "defaults.h"
#include "defaults-universal.h"
// userDefault.h is like the above, but with a default template for the user's settings.json.
#include "userDefaults.h"
// Both defaults.h and userDefaults.h are generated at build time into the
// "Generated Files" directory.

#include "ApplicationState.h"
#include "FileUtils.h"

using namespace winrt::Microsoft::Terminal::Settings::Model::implementation;
using namespace ::Microsoft::Console;
using namespace ::Microsoft::Terminal::Settings::Model;

static constexpr std::wstring_view SettingsFilename{ L"settings.json" };
static constexpr std::wstring_view DefaultsFilename{ L"defaults.json" };

static constexpr std::string_view ProfilesKey{ "profiles" };
static constexpr std::string_view DefaultSettingsKey{ "defaults" };
static constexpr std::string_view ProfilesListKey{ "list" };
static constexpr std::string_view SchemesKey{ "schemes" };
static constexpr std::string_view NameKey{ "name" };
static constexpr std::string_view UpdatesKey{ "updates" };
static constexpr std::string_view GuidKey{ "guid" };

static constexpr std::wstring_view jsonExtension{ L".json" };
static constexpr std::wstring_view FragmentsSubDirectory{ L"\\Fragments" };
static constexpr std::wstring_view FragmentsPath{ L"\\Microsoft\\Windows Terminal\\Fragments" };

static constexpr std::string_view AppExtensionHostName{ "com.microsoft.windows.terminal.settings" };

// Function Description:
// - Extracting the value from an async task (like talking to the app catalog) when we are on the
//   UI thread causes C++/WinRT to complain quite loudly (and halt execution!)
//   This templated function extracts the result from a task with chicanery.
template<typename TTask>
static auto extractValueFromTaskWithoutMainThreadAwait(TTask&& task) -> decltype(task.get())
{
    std::optional<decltype(task.get())> finalVal;
    til::latch latch{ 1 };

    const auto _ = [&]() -> winrt::fire_and_forget {
        co_await winrt::resume_background();
        finalVal.emplace(co_await task);
        latch.count_down();
    }();

    latch.wait();
    return finalVal.value();
}

static std::pair<size_t, size_t> lineAndColumnFromPosition(const std::string_view string, const size_t position)
{
    size_t line = 1;
    size_t pos = 0;

    for (;;)
    {
        const auto p = string.find('\n', pos);
        if (p >= position)
        {
            break;
        }

        pos = p + 1;
        line++;
    }

    return { line, position - pos + 1 };
}

static void catchRethrowSerializationExceptionWithLocationInfo(std::string_view settingsString)
{
    std::string msg;

    try
    {
        throw;
    }
    catch (const JsonUtils::DeserializationError& e)
    {
        static constexpr std::string_view basicHeader{ "* Line {line}, Column {column}\n{message}" };
        static constexpr std::string_view keyedHeader{ "* Line {line}, Column {column} ({key})\n{message}" };

        std::string jsonValueAsString{ "array or object" };
        try
        {
            jsonValueAsString = e.jsonValue.asString();
            if (e.jsonValue.isString())
            {
                jsonValueAsString = fmt::format("\"{}\"", jsonValueAsString);
            }
        }
        catch (...)
        {
            // discard: we're in the middle of error handling
        }

        msg = fmt::format("  Have: {}\n  Expected: {}", jsonValueAsString, e.expectedType);

        auto [l, c] = lineAndColumnFromPosition(settingsString, static_cast<size_t>(e.jsonValue.getOffsetStart()));
        msg = fmt::format((e.key ? keyedHeader : basicHeader),
                          fmt::arg("line", l),
                          fmt::arg("column", c),
                          fmt::arg("key", e.key.value_or("")),
                          fmt::arg("message", msg));
        throw SettingsTypedDeserializationException{ msg };
    }
}

// Method Description:
// - Creates a CascadiaSettings from whatever's saved on disk, or instantiates
//      a new one with the default values. If we're running as a packaged app,
//      it will load the settings from our packaged localappdata. If we're
//      running as an unpackaged application, it will read it from the path
//      we've set under localappdata.
// - Loads both the settings from the defaults.json and the user's settings.json
// - Also runs and dynamic profile generators. If any of those generators create
//   new profiles, we'll write the user settings back to the file, with the new
//   profiles inserted into their list of profiles.
// Return Value:
// - a unique_ptr containing a new CascadiaSettings object.
winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings CascadiaSettings::LoadAll()
{
    try
    {
        const auto settings = LoadDefaults();
        const auto resultPtr = winrt::get_self<CascadiaSettings>(settings);
        resultPtr->ClearWarnings();

        auto settingsString = ReadUTF8FileIfExists(_SettingsPath()).value_or(std::string{});

        Json::Value userSettings;
        bool needToWriteFile = false;

        // Make sure the file isn't totally empty. If it is, we'll treat the file
        // like it doesn't exist at all.
        if (!settingsString.empty())
        {
            userSettings = _ParseUtf8JsonString(settingsString);
        }
        else
        {
            // We didn't find the user settings. We'll need to create a file
            // to use as the user defaults.
            // For now, just parse our user settings template as their user settings.
            settingsString = resultPtr->_ApplyFirstRunChangesToSettingsTemplate(UserSettingsJson);
            userSettings = _ParseUtf8JsonString(settingsString);
            needToWriteFile = true;
        }

        // Load profiles from dynamic profile generators. _userSettings should be
        // created by now, because we're going to check in there for any generators
        // that should be disabled (if the user had any settings.)
        try
        {
            std::unordered_set<std::wstring> ignoredNamespaces;
            if (const auto sources = resultPtr->_globals->DisabledProfileSources())
            {
                ignoredNamespaces.reserve(sources.Size());
                for (const auto& id : sources)
                {
                    ignoredNamespaces.emplace(id);
                }
            }

            resultPtr->_LoadDynamicProfiles(ignoredNamespaces);
            resultPtr->_LoadFragmentExtensions(ignoredNamespaces);
        }
        CATCH_LOG();

        try
        {
            // See microsoft/terminal#2325: find the defaultSettings from the user's
            // settings. Layer those settings upon all the existing profiles we have
            // (defaults and dynamic profiles). We'll also set
            // _userDefaultProfileSettings here. When we LayerJson below to apply the
            // user settings, we'll make sure to use these defaultSettings _before_ any
            // profiles the user might have.
            resultPtr->_ApplyDefaultsFromUserSettings(userSettings);

            // Apply the user's settings
            resultPtr->LayerJson(userSettings);
        }
        catch (...)
        {
            catchRethrowSerializationExceptionWithLocationInfo(settingsString);
        }

        // Let's say a user doesn't know that they need to write `"hidden": true` in
        // order to prevent a profile from showing up (and a settings UI doesn't exist).
        // Naturally they would open settings.json and try to remove the profile object.
        // This section of code recognizes if a profile was seen before and marks it as
        // `"hidden": true` by default and thus ensures the behavior the user expects:
        // Profiles won't show up again after they've been removed from settings.json.
        {
            const auto state = winrt::get_self<implementation::ApplicationState>(ApplicationState::SharedInstance());
            auto generatedProfiles = state->GeneratedProfiles();
            bool generatedProfilesChanged = false;

            for (auto profile : resultPtr->_allProfiles)
            {
                if (generatedProfiles.emplace(profile.Guid()).second)
                {
                    generatedProfilesChanged = true;
                }
                else if (profile.Origin() != OriginTag::User)
                {
                    profile.Hidden(true);
                }
            }

            if (generatedProfilesChanged)
            {
                state->GeneratedProfiles(generatedProfiles);
            }
        }

        // TODO:GH#2721 If powershell core is installed, we need to set that to the
        // default profile, but only when the settings file was newly created. We'll
        // re-write the segment of the user settings for "default profile" to have
        // the powershell core GUID instead.

        // If we created the file, or found new dynamic profiles, write the user
        // settings string back to the file.
        if (needToWriteFile)
        {
            try
            {
                resultPtr->WriteSettingsToDisk();
            }
            catch (...)
            {
                LOG_CAUGHT_EXCEPTION();
                resultPtr->AppendWarning(SettingsLoadWarnings::FailedToWriteToSettings);
            }
        }

        // If this throws, the app will catch it and use the default settings
        resultPtr->_ValidateSettings();

        return *resultPtr;
    }
    catch (const SettingsException& ex)
    {
        auto settings{ winrt::make_self<implementation::CascadiaSettings>() };
        settings->_loadError = ex.Error();
        return *settings;
    }
    catch (const SettingsTypedDeserializationException& e)
    {
        auto settings{ winrt::make_self<implementation::CascadiaSettings>() };
        std::string_view what{ e.what() };
        settings->_deserializationErrorMessage = til::u8u16(what);
        return *settings;
    }
}

// Function Description:
// - Loads a batch of settings curated for the Universal variant of the terminal app
// Arguments:
// - <none>
// Return Value:
// - a unique_ptr to a CascadiaSettings with the connection types and settings for Universal terminal
winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings CascadiaSettings::LoadUniversal()
{
    // We're going to do this ourselves because we want to exclude almost everything
    // from the special Universal-for-developers configuration

    try
    {
        // Create settings and get the universal defaults loaded up.
        auto resultPtr = winrt::make_self<CascadiaSettings>();
        resultPtr->LayerJson(_ParseUtf8JsonString(DefaultUniversalJson));

        // Now validate.
        // If this throws, the app will catch it and use the default settings
        resultPtr->_ValidateSettings();

        return *resultPtr;
    }
    catch (const SettingsException& ex)
    {
        auto settings{ winrt::make_self<implementation::CascadiaSettings>() };
        settings->_loadError = ex.Error();
        return *settings;
    }
    catch (const SettingsTypedDeserializationException& e)
    {
        auto settings{ winrt::make_self<implementation::CascadiaSettings>() };
        std::string_view what{ e.what() };
        settings->_deserializationErrorMessage = til::u8u16(what);
        return *settings;
    }
}

// Function Description:
// - Creates a new CascadiaSettings object initialized with settings from the
//   hardcoded defaults.json.
// Arguments:
// - <none>
// Return Value:
// - a unique_ptr to a CascadiaSettings with the settings from defaults.json
winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings CascadiaSettings::LoadDefaults()
{
    auto resultPtr{ winrt::make_self<CascadiaSettings>() };
    resultPtr->LayerJson(_ParseUtf8JsonString(DefaultJson));
    resultPtr->_UpdateActiveProfiles();
    resultPtr->_ResolveDefaultProfile();

    // tag these profiles as in-box
    for (const auto& profile : resultPtr->AllProfiles())
    {
        const auto& profileImpl{ winrt::get_self<implementation::Profile>(profile) };
        profileImpl->Origin(OriginTag::InBox);
    }

    return *resultPtr;
}

// Method Description:
// - Runs each of the configured dynamic profile generators (DPGs). Adds
//   profiles from any DPGs that ran to the end of our list of profiles.
// - Uses the Json::Value _userSettings to check which DPGs should not be run.
//   If the user settings has any namespaces in the "disabledProfileSources"
//   property, we'll ensure that any DPGs with a matching namespace _don't_ run.
// Arguments:
// - <none>
// Return Value:
// - <none>
void CascadiaSettings::_LoadDynamicProfiles(const std::unordered_set<std::wstring>& ignoredNamespaces)
{
    for (const auto& generator : _profileGenerators)
    {
        const std::wstring generatorNamespace{ generator->GetNamespace() };

        if (ignoredNamespaces.find(generatorNamespace) == ignoredNamespaces.end())
        {
            try
            {
                auto profiles = generator->GenerateProfiles();
                for (auto& profile : profiles)
                {
                    profile.Source(generatorNamespace);
                    _allProfiles.Append(profile);
                }
            }
            CATCH_LOG_MSG("Dynamic Profile Namespace: \"%s\"", generatorNamespace.c_str());
        }
    }
}

// Method Description:
// - Searches the local app data folder, global app data folder and app
//   extensions for json stubs we should use to create new profiles,
//   modify existing profiles or add new color schemes
// - If the user settings has any namespaces in the "disabledProfileSources"
//   property, we'll ensure that the corresponding folders do not get searched
void CascadiaSettings::_LoadFragmentExtensions(const std::unordered_set<std::wstring>& ignoredNamespaces)
{
    for (const auto& rfid : std::array{ FOLDERID_LocalAppData, FOLDERID_ProgramData })
    {
        wil::unique_cotaskmem_string folder;
        THROW_IF_FAILED(SHGetKnownFolderPath(rfid, 0, nullptr, &folder));

        std::wstring fragmentPath{ folder.get() };
        fragmentPath.append(FragmentsPath);

        if (std::filesystem::exists(fragmentPath))
        {
            _ApplyJsonStubsHelper(fragmentPath, ignoredNamespaces);
        }
    }

    // Search through app extensions
    // Gets the catalog of extensions with the name "com.microsoft.windows.terminal.settings"
    const auto catalog = Windows::ApplicationModel::AppExtensions::AppExtensionCatalog::Open(winrt::to_hstring(AppExtensionHostName));

    auto extensions = extractValueFromTaskWithoutMainThreadAwait(catalog.FindAllAsync());

    for (const auto& ext : extensions)
    {
        // Only apply the stubs if the package name is not in ignored namespaces
        const std::wstring familyName{ ext.Package().Id().FamilyName() };
        if (ignoredNamespaces.find(familyName) == ignoredNamespaces.end())
        {
            // Likewise, getting the public folder from an extension is an async operation
            // So we use another mutex and condition variable
            auto foundFolder = extractValueFromTaskWithoutMainThreadAwait(ext.GetPublicFolderAsync());

            if (foundFolder)
            {
                // the StorageFolder class has its own methods for obtaining the files within the folder
                // however, all those methods are Async methods
                // you may have noticed that we need to resort to clunky implementations for async operations
                // (they are in extractValueFromTaskWithoutMainThreadAwait)
                // so for now we will just take the folder path and access the files that way
                std::wstring path{ foundFolder.Path() };
                path.append(FragmentsSubDirectory);

                // If the directory exists, use the fragments in it
                if (std::filesystem::exists(path))
                {
                    const auto jsonFiles = _AccumulateJsonFilesInDirectory(path);

                    // Provide the package name as the source
                    _ParseAndLayerFragmentFiles(jsonFiles, ext.Package().Id().FamilyName());
                }
            }
        }
    }
}

// Method Description:
// - Helper function to apply json stubs in the local app data folder and the global program data folder
// Arguments:
// - The directory to find json files in
// - The set of ignored namespaces
void CascadiaSettings::_ApplyJsonStubsHelper(const std::wstring_view directory, const std::unordered_set<std::wstring>& ignoredNamespaces)
{
    // The json files should be within subdirectories where the subdirectory name is the app name
    for (const auto& fragmentExtFolder : std::filesystem::directory_iterator(directory))
    {
        // We only want the parent folder name as the source (not the full path)
        const auto filename = fragmentExtFolder.path().filename();
        const auto& source = filename.native();

        // Only apply the stubs if the parent folder name is not in ignored namespaces
        // (also make sure this is a directory for sanity)
        if (std::filesystem::is_directory(fragmentExtFolder) && ignoredNamespaces.find(source) == ignoredNamespaces.end())
        {
            const auto jsonFiles = _AccumulateJsonFilesInDirectory(fragmentExtFolder.path().native());
            _ParseAndLayerFragmentFiles(jsonFiles, winrt::hstring{ source });
        }
    }
}

// Method Description:
// - Finds all the json files within the given directory
// Arguments:
// - directory: the directory to search
// Return Value:
// - A set containing all the found file data
std::unordered_set<std::string> CascadiaSettings::_AccumulateJsonFilesInDirectory(const std::wstring_view directory)
{
    std::unordered_set<std::string> jsonFiles;

    for (const auto& fragmentExt : std::filesystem::directory_iterator(directory))
    {
        if (fragmentExt.path().extension() == jsonExtension)
        {
            try
            {
                jsonFiles.emplace(ReadUTF8File(fragmentExt.path()));
            }
            CATCH_LOG();
        }
    }
    return jsonFiles;
}

// Method Description:
// - Given a set of json files, uses them to modify existing profiles,
//   create new profiles, and create new color schemes
// Arguments:
// - files: the set of json files (each item in the set is the file data)
// - source: the location the files came from
void CascadiaSettings::_ParseAndLayerFragmentFiles(const std::unordered_set<std::string> files, const winrt::hstring source)
{
    for (const auto& file : files)
    {
        // A file could have many new profiles/many profiles it wants to modify/many new color schemes
        // so we first parse the entire file into one json object
        auto fullFile = _ParseUtf8JsonString(file);

        if (fullFile.isMember(JsonKey(ProfilesKey)))
        {
            // Now we separately get each stub that modifies/adds a profile
            // We intentionally don't use a const reference here because we modify
            // the profile stub by giving it a guid so we can call _FindMatchingProfile
            for (auto& profileStub : fullFile[JsonKey(ProfilesKey)])
            {
                if (profileStub.isMember(JsonKey(UpdatesKey)))
                {
                    // This stub is meant to be a modification to an existing profile,
                    // try to find the matching profile
                    profileStub[JsonKey(GuidKey)] = profileStub[JsonKey(UpdatesKey)];
                    auto matchingProfile = _FindMatchingProfile(profileStub);
                    if (matchingProfile)
                    {
                        try
                        {
                            // We found a matching profile, create a child of it and put the modifications there
                            // (we add a new inheritance layer)
                            auto childImpl{ matchingProfile->CreateChild() };
                            childImpl->LayerJson(profileStub);
                            childImpl->Origin(OriginTag::Fragment);

                            // replace parent in _profiles with child
                            _allProfiles.SetAt(_FindMatchingProfileIndex(matchingProfile->ToJson()).value(), *childImpl);
                        }
                        catch (...)
                        {
                        }
                    }
                }
                else
                {
                    // This is a new profile, check that it meets our minimum requirements first
                    // (it must have at least a name)
                    if (profileStub.isMember(JsonKey(NameKey)))
                    {
                        try
                        {
                            auto newProfile = Profile::FromJson(profileStub);
                            // Make sure to give the new profile a source, then we add it to our list of profiles
                            // We don't make modifications to the user's settings file yet, that will happen when
                            // _AppendDynamicProfilesToUserSettings() is called later
                            newProfile->Source(source);
                            newProfile->Origin(OriginTag::Fragment);
                            _allProfiles.Append(*newProfile);
                        }
                        catch (...)
                        {
                        }
                    }
                }
            }
        }

        if (fullFile.isMember(JsonKey(SchemesKey)))
        {
            // Now we separately get each stub that adds a color scheme
            for (const auto& schemeStub : fullFile[JsonKey(SchemesKey)])
            {
                if (_FindMatchingColorScheme(schemeStub))
                {
                    // We do not allow modifications to existing color schemes
                }
                else
                {
                    // This is a new color scheme, add it only if it specifies _all_ the fields
                    if (ColorScheme::ValidateColorScheme(schemeStub))
                    {
                        const auto newScheme = ColorScheme::FromJson(schemeStub);
                        _globals->AddColorScheme(*newScheme);
                    }
                }
            }
        }
    }
}

// Method Description:
// - Attempts to read the given data as a string of JSON and parse that JSON
//   into a Json::Value
// - Will ignore leading UTF-8 BOMs
// Arguments:
// - fileData: the string to parse as JSON data
// Return value:
// - the parsed json value
Json::Value CascadiaSettings::_ParseUtf8JsonString(std::string_view fileData)
{
    Json::Value result;
    const auto actualDataStart = fileData.data();
    const auto actualDataEnd = fileData.data() + fileData.size();

    std::string errs; // This string will receive any error text from failing to parse.
    std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder::CharReaderBuilder().newCharReader() };

    // `parse` will return false if it fails.
    if (!reader->parse(actualDataStart, actualDataEnd, &result, &errs))
    {
        // This will be caught by App::_TryLoadSettings, who will display
        // the text to the user.
        throw winrt::hresult_error(WEB_E_INVALID_JSON_STRING, winrt::to_hstring(errs));
    }
    return result;
}

// Function Description:
// - Given a json serialization of a profile, this function will determine
//   whether it is "well-formed". We introduced a bug (GH#9962, fixed in GH#9964)
//   that would result in one or more nameless, guid-less profiles being emitted
//   into the user's settings file. Those profiles would show up in the list as
//   "Default" later.
static bool _IsValidProfileObject(const Json::Value& profileJson)
{
    return profileJson.isMember(NameKey.data(), NameKey.data() + NameKey.size()) || // has a name (can generate a guid)
           profileJson.isMember(GuidKey.data(), GuidKey.data() + GuidKey.size()); // or has a guid
}

// Method Description:
// - Create a new instance of this class from a serialized JsonObject.
// Arguments:
// - json: an object which should be a serialization of a CascadiaSettings object.
// Return Value:
// - a new CascadiaSettings instance created from the values in `json`
winrt::com_ptr<CascadiaSettings> CascadiaSettings::FromJson(const Json::Value& json)
{
    auto resultPtr = winrt::make_self<CascadiaSettings>();
    resultPtr->LayerJson(json);
    return resultPtr;
}

// Method Description:
// - Layer values from the given json object on top of the existing properties
//   of this object. For any keys we're expecting to be able to parse in the
//   given object, we'll parse them and replace our settings with values from
//   the new json object. Properties that _aren't_ in the json object will _not_
//   be replaced.
// Arguments:
// - json: an object which should be a partial serialization of a CascadiaSettings object.
// Return Value:
// <none>
void CascadiaSettings::LayerJson(const Json::Value& json)
{
    // add a new inheritance layer, and apply json values to child
    _globals = _globals->CreateChild();
    _globals->LayerJson(json);

    if (auto schemes{ json[JsonKey(SchemesKey)] })
    {
        for (auto schemeJson : schemes)
        {
            if (schemeJson.isObject())
            {
                _LayerOrCreateColorScheme(schemeJson);
            }
        }
    }

    for (auto profileJson : _GetProfilesJsonObject(json))
    {
        if (profileJson.isObject() && _IsValidProfileObject(profileJson))
        {
            _LayerOrCreateProfile(profileJson);
        }
    }
}

// Method Description:
// - Given a partial json serialization of a Profile object, either layers that
//   json on a matching Profile we already have, or creates a new Profile
//   object from those settings.
// - For profiles that were created from a dynamic profile source, they'll have
//   both a guid and source guid that must both match. If a user profile with a
//   source set does not find a matching profile at load time, the profile
//   should be ignored.
// Arguments:
// - json: an object which may be a partial serialization of a Profile object.
// Return Value:
// - <none>
void CascadiaSettings::_LayerOrCreateProfile(const Json::Value& profileJson)
{
    // Layer the json on top of an existing profile, if we have one:
    winrt::com_ptr<implementation::Profile> profile{ nullptr };
    auto profileIndex{ _FindMatchingProfileIndex(profileJson) };
    if (profileIndex)
    {
        auto parentProj{ _allProfiles.GetAt(*profileIndex) };
        auto parent{ winrt::get_self<Profile>(parentProj) };

        if (_userDefaultProfileSettings)
        {
            // We don't actually need to CreateChild() here.
            // When we loaded Profile.Defaults, we created an empty child already.
            // So this just populates the empty child
            parent->LayerJson(profileJson);
            profile.copy_from(parent);
        }
        else
        {
            // otherwise, add a new inheritance layer
            auto childImpl{ parent->CreateChild() };
            childImpl->LayerJson(profileJson);

            // replace parent in _profiles with child
            _allProfiles.SetAt(*profileIndex, *childImpl);
            profile = std::move(childImpl);
        }
    }
    else
    {
        // If this JSON represents a dynamic profile, we _shouldn't_ create the
        // profile here. We only want to create profiles for profiles without a
        // `source`. Dynamic profiles _must_ be layered on an existing profile.
        if (!Profile::IsDynamicProfileObject(profileJson))
        {
            profile = winrt::make_self<Profile>();

            // GH#2325: If we have a set of default profile settings, set that as my parent.
            // We _won't_ have these settings yet for defaults, dynamic profiles.
            if (_userDefaultProfileSettings)
            {
                Profile::InsertParentHelper(profile, _userDefaultProfileSettings, 0);
            }

            profile->LayerJson(profileJson);
            _allProfiles.Append(*profile);
        }
    }

    if (profile && _userDefaultProfileSettings)
    {
        // If we've loaded defaults{} we're in the "user settings" phase for sure
        profile->Origin(OriginTag::User);
    }
}

// Method Description:
// - Finds a profile from our list of profiles that matches the given json
//   object. Uses Profile::ShouldBeLayered to determine if the Json::Value is a
//   match or not. This method should be used to find a profile to layer the
//   given settings upon.
// - Returns nullptr if no such match exists.
// Arguments:
// - json: an object which may be a partial serialization of a Profile object.
// Return Value:
// - a Profile that can be layered with the given json object, iff such a
//   profile exists.
winrt::com_ptr<Profile> CascadiaSettings::_FindMatchingProfile(const Json::Value& profileJson)
{
    auto index{ _FindMatchingProfileIndex(profileJson) };
    if (index)
    {
        auto profile{ _allProfiles.GetAt(*index) };
        auto profileImpl{ winrt::get_self<Profile>(profile) };
        return profileImpl->get_strong();
    }
    return nullptr;
}

// Method Description:
// - Finds a profile from our list of profiles that matches the given json
//   object. Uses Profile::ShouldBeLayered to determine if the Json::Value is a
//   match or not. This method should be used to find a profile to layer the
//   given settings upon.
// - Returns nullopt if no such match exists.
// Arguments:
// - json: an object which may be a partial serialization of a Profile object.
// Return Value:
// - The index for the matching Profile, iff it exists. Otherwise, nullopt.
std::optional<uint32_t> CascadiaSettings::_FindMatchingProfileIndex(const Json::Value& profileJson)
{
    for (uint32_t i = 0; i < _allProfiles.Size(); ++i)
    {
        const auto profile{ _allProfiles.GetAt(i) };
        const auto profileImpl = winrt::get_self<Profile>(profile);
        if (profileImpl->ShouldBeLayered(profileJson))
        {
            return i;
        }
    }
    return std::nullopt;
}

// Method Description:
// - Finds the "default profile settings" if they exist in the users settings,
//   and applies them to the existing profiles. The "default profile settings"
//   are settings that should be applied to every profile a user has, with the
//   option of being overridden by explicit values in the profile. This should
//   be called _after_ the defaults have been parsed and dynamic profiles have
//   been generated, but before the other user profiles have been loaded.
// Arguments:
// - <none>
// Return Value:
// - <none>
void CascadiaSettings::_ApplyDefaultsFromUserSettings(const Json::Value& userSettings)
{
    // If `profiles` was an object, then look for the `defaults` object
    // underneath it for the default profile settings.
    // If there isn't one, we still want to add an empty "default" profile to the inheritance tree.
    Json::Value defaultSettings{ Json::ValueType::objectValue };
    if (const auto profiles{ userSettings[JsonKey(ProfilesKey)] })
    {
        if (profiles.isObject() && !profiles[JsonKey(DefaultSettingsKey)].empty())
        {
            defaultSettings = profiles[JsonKey(DefaultSettingsKey)];
        }
    }

    _userDefaultProfileSettings = winrt::make_self<Profile>();
    _userDefaultProfileSettings->LayerJson(defaultSettings);
    // Remove the `guid` member from the default settings.
    // That'll hyper-explode, so just don't let them do that.
    _userDefaultProfileSettings->ClearGuid();
    _userDefaultProfileSettings->Origin(OriginTag::ProfilesDefaults);

    const auto numOfProfiles{ _allProfiles.Size() };
    for (uint32_t profileIndex = 0; profileIndex < numOfProfiles; ++profileIndex)
    {
        // create a child, so we inherit from the defaults.json layer
        auto parentProj{ _allProfiles.GetAt(profileIndex) };
        auto parentImpl{ winrt::get_self<Profile>(parentProj) };
        auto childImpl{ parentImpl->CreateChild() };

        // Add profile.defaults as the _first_ parent to the child
        Profile::InsertParentHelper(childImpl, _userDefaultProfileSettings, 0);

        // replace parent in _profiles with child
        _allProfiles.SetAt(profileIndex, *childImpl);
    }
}

// Method Description:
// - Given a partial json serialization of a ColorScheme object, either layers that
//   json on a matching ColorScheme we already have, or creates a new ColorScheme
//   object from those settings.
// Arguments:
// - json: an object which should be a partial serialization of a ColorScheme object.
// Return Value:
// - <none>
void CascadiaSettings::_LayerOrCreateColorScheme(const Json::Value& schemeJson)
{
    // Layer the json on top of an existing profile, if we have one:
    auto pScheme = _FindMatchingColorScheme(schemeJson);
    if (pScheme)
    {
        pScheme->LayerJson(schemeJson);
    }
    else
    {
        const auto scheme = ColorScheme::FromJson(schemeJson);
        _globals->AddColorScheme(*scheme);
    }
}

// Method Description:
// - Finds a color scheme from our list of color schemes that matches the given
//   json object. Uses ColorScheme::GetNameFromJson to find the name and then
//   performs a lookup in the global map. This method should be used to find a
//   color scheme to layer the given settings upon.
// - Returns nullptr if no such match exists.
// Arguments:
// - json: an object which should be a partial serialization of a ColorScheme object.
// Return Value:
// - a ColorScheme that can be layered with the given json object, iff such a
//   color scheme exists.
winrt::com_ptr<ColorScheme> CascadiaSettings::_FindMatchingColorScheme(const Json::Value& schemeJson)
{
    if (auto schemeName = ColorScheme::GetNameFromJson(schemeJson))
    {
        if (auto scheme{ _globals->ColorSchemes().TryLookup(*schemeName) })
        {
            return winrt::get_self<ColorScheme>(scheme)->get_strong();
        }
    }
    return nullptr;
}

// Method Description:
// - Returns the path of the settings.json file.
// Arguments:
// - <none>
// Return Value:
// - Returns a path in 80% of cases. I measured!
const std::filesystem::path& CascadiaSettings::_SettingsPath()
{
    static const auto path = GetBaseSettingsPath() / SettingsFilename;
    return path;
}

// function Description:
// - Returns the full path to the settings file, either within the application
//   package, or in its unpackaged location. This path is under the "Local
//   AppData" folder, so it _doesn't_ roam to other machines.
// - If the application is unpackaged,
//   the file will end up under e.g. C:\Users\admin\AppData\Local\Microsoft\Windows Terminal\settings.json
// Arguments:
// - <none>
// Return Value:
// - the full path to the settings file
winrt::hstring CascadiaSettings::SettingsPath()
{
    return winrt::hstring{ _SettingsPath().wstring() };
}

winrt::hstring CascadiaSettings::DefaultSettingsPath()
{
    // Both of these posts suggest getting the path to the exe, then removing
    // the exe's name to get the package root:
    // * https://blogs.msdn.microsoft.com/appconsult/2017/06/23/accessing-to-the-files-in-the-installation-folder-in-a-desktop-bridge-application/
    // * https://blogs.msdn.microsoft.com/appconsult/2017/03/06/handling-data-in-a-converted-desktop-app-with-the-desktop-bridge/
    //
    // This would break if we ever moved our exe out of the package root.
    // HOWEVER, if we try to look for a defaults.json that's simply in the same
    // directory as the exe, that will work for unpackaged scenarios as well. So
    // let's try that.

    std::wstring exePathString;
    THROW_IF_FAILED(wil::GetModuleFileNameW(nullptr, exePathString));

    std::filesystem::path path{ exePathString };
    path.replace_filename(DefaultsFilename);
    return winrt::hstring{ path.wstring() };
}

// Function Description:
// - Gets the object in the given JSON object under the "profiles" key. Returns
//   null if there's no "profiles" key.
// Arguments:
// - json: the json object to get the profiles from.
// Return Value:
// - the Json::Value representing the profiles property from the given object
const Json::Value& CascadiaSettings::_GetProfilesJsonObject(const Json::Value& json)
{
    const auto& profilesProperty = json[JsonKey(ProfilesKey)];
    return profilesProperty.isArray() ?
               profilesProperty :
               profilesProperty[JsonKey(ProfilesListKey)];
}

// Method Description:
// - Write the current state of CascadiaSettings to our settings file
// - Create a backup file with the current contents, if one does not exist
// - Persists the default terminal handler choice to the registry
// Arguments:
// - <none>
// Return Value:
// - <none>
void CascadiaSettings::WriteSettingsToDisk() const
{
    const auto settingsPath = _SettingsPath();

    {
        // create a timestamped backup file
        const auto backupSettingsPath = fmt::format(L"{}.{:%Y-%m-%dT%H-%M-%S}.backup", settingsPath.native(), fmt::localtime(std::time(nullptr)));
        LOG_IF_WIN32_BOOL_FALSE(CopyFileW(settingsPath.c_str(), backupSettingsPath.c_str(), TRUE));
    }

    // write current settings to current settings file
    Json::StreamWriterBuilder wbuilder;
    wbuilder.settings_["indentation"] = "    ";
    wbuilder.settings_["enableYAMLCompatibility"] = true; // suppress spaces around colons

    const auto styledString{ Json::writeString(wbuilder, ToJson()) };
    WriteUTF8FileAtomic(settingsPath, styledString);

    // Persists the default terminal choice
    //
    // GH#10003 - Only do this if _currentDefaultTerminal was actually
    // initialized. It's only initialized when Launch.cpp calls
    // `CascadiaSettings::RefreshDefaultTerminals`. We really don't need it
    // otherwise.
    if (_currentDefaultTerminal)
    {
        Model::DefaultTerminal::Current(_currentDefaultTerminal);
    }
}

// Method Description:
// - Create a new serialized JsonObject from an instance of this class
// Arguments:
// - <none>
// Return Value:
// the JsonObject representing this instance
Json::Value CascadiaSettings::ToJson() const
{
    // top-level json object
    Json::Value json{ _globals->ToJson() };
    json["$help"] = "https://aka.ms/terminal-documentation";
    json["$schema"] = "https://aka.ms/terminal-profiles-schema";

    // "profiles" will always be serialized as an object
    Json::Value profiles{ Json::ValueType::objectValue };
    profiles[JsonKey(DefaultSettingsKey)] = _userDefaultProfileSettings ? _userDefaultProfileSettings->ToJson() :
                                                                          Json::ValueType::objectValue;
    Json::Value profilesList{ Json::ValueType::arrayValue };
    for (const auto& entry : _allProfiles)
    {
        const auto prof{ winrt::get_self<implementation::Profile>(entry) };
        profilesList.append(prof->ToJson());
    }
    profiles[JsonKey(ProfilesListKey)] = profilesList;
    json[JsonKey(ProfilesKey)] = profiles;

    // TODO GH#8100:
    // "schemes" will be an accumulation of _all_ the color schemes
    // including all of the ones from defaults.json
    Json::Value schemes{ Json::ValueType::arrayValue };
    for (const auto& entry : _globals->ColorSchemes())
    {
        const auto scheme{ winrt::get_self<implementation::ColorScheme>(entry.Value()) };
        schemes.append(scheme->ToJson());
    }
    json[JsonKey(SchemesKey)] = schemes;

    return json;
}
