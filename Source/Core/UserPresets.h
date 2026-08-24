#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <optional>

/*
    UNI 76 - user preset storage.

    A user preset is the same 8 parameters + 7 module-enable flags a
    factory preset (Core/FactoryPresets.h) carries, just captured from the
    processor's live state instead of hand-written, and persisted to disk
    as one small XML file per preset under the standard per-user
    application-data directory - never a hardcoded absolute path (see
    getUserPresetsDirectory()). Not a new saved-state format for the
    plugin's own getStateInformation()/setStateInformation() - these files
    are only ever read on user gesture (opening the PRESET menu, clicking
    Save/Load/Delete), all on the message thread, never the audio thread.
*/
namespace uni76
{
    struct UserPresetData
    {
        std::array<float, 8> values {};       // ParamID::all order, real units
        std::array<bool, 7> moduleEnabled {};
    };

    inline juce::File getUserPresetsDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::SpecialLocationType::userApplicationDataDirectory)
                   .getChildFile ("Nostalgia Audio")
                   .getChildFile ("UNI 76")
                   .getChildFile ("Presets")
                   .getChildFile ("User");
    }

    // Conservative filesystem-safe filename: keep alphanumerics/space/dash/
    // underscore, drop everything else (path separators, quotes, control
    // chars, etc.) so a user-typed preset name can never escape the presets
    // directory or collide with reserved characters on Windows/macOS.
    inline juce::String sanitizeUserPresetFilename (const juce::String& name)
    {
        juce::String result;
        for (auto c : name)
        {
            if (juce::CharacterFunctions::isLetterOrDigit (c) || c == ' ' || c == '-' || c == '_')
                result += juce::String::charToString (c);
        }
        result = result.trim();
        return result.isEmpty() ? "Untitled" : result;
    }

    inline juce::File getUserPresetFile (const juce::String& name)
    {
        return getUserPresetsDirectory().getChildFile (sanitizeUserPresetFilename (name) + ".uni76preset");
    }

    constexpr int userPresetSchemaVersion = 1;

    // Directory scan only - call on user gesture (PRESET menu open), never
    // at editor construction, so it never sits on the cold-open critical
    // path.
    inline juce::StringArray listUserPresetNames()
    {
        juce::StringArray names;
        auto dir = getUserPresetsDirectory();
        if (! dir.isDirectory())
            return names;

        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*.uni76preset", juce::File::findFiles))
            names.add (entry.getFile().getFileNameWithoutExtension());

        names.sort (true);
        return names;
    }

    inline bool saveUserPreset (const juce::String& name, const UserPresetData& data)
    {
        auto dir = getUserPresetsDirectory();
        if (! dir.isDirectory() && ! dir.createDirectory())
            return false;

        juce::XmlElement root ("UNI76UserPreset");
        root.setAttribute ("schemaVersion", userPresetSchemaVersion);
        root.setAttribute ("name", name);

        auto* params = root.createNewChildElement ("Parameters");
        static const char* paramNames[8] { "preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager", "imageTilt" };
        for (int i = 0; i < 8; ++i)
            params->setAttribute (paramNames[i], (double) data.values[(size_t) i]);

        auto* modules = root.createNewChildElement ("ModulesEnabled");
        static const char* moduleNames[7] { "preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager" };
        for (int i = 0; i < 7; ++i)
            modules->setAttribute (moduleNames[i], data.moduleEnabled[(size_t) i]);

        return root.writeTo (getUserPresetFile (name));
    }

    inline std::optional<UserPresetData> loadUserPreset (const juce::String& name)
    {
        auto file = getUserPresetFile (name);
        if (! file.existsAsFile())
            return std::nullopt;

        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr || xml->getTagName() != "UNI76UserPreset")
            return std::nullopt;

        auto* params = xml->getChildByName ("Parameters");
        auto* modules = xml->getChildByName ("ModulesEnabled");
        if (params == nullptr || modules == nullptr)
            return std::nullopt;

        UserPresetData data;
        static const char* paramNames[8] { "preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager", "imageTilt" };
        for (int i = 0; i < 8; ++i)
            data.values[(size_t) i] = (float) params->getDoubleAttribute (paramNames[i]);

        static const char* moduleNames[7] { "preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager" };
        for (int i = 0; i < 7; ++i)
            data.moduleEnabled[(size_t) i] = modules->getBoolAttribute (moduleNames[i], true);

        return data;
    }

    inline bool deleteUserPreset (const juce::String& name)
    {
        auto file = getUserPresetFile (name);
        return ! file.existsAsFile() || file.deleteFile();
    }

    inline bool userPresetExists (const juce::String& name)
    {
        return getUserPresetFile (name).existsAsFile();
    }
}
