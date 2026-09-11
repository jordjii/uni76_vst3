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
        std::array<float, 9> values {};       // ParamID::all order, real units
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

    // Keeps a saved preset's name inside the header's fixed-width PRESET
    // label without relying on the ellipsis to hide most of it - see
    // header_controls.js's buildSaveRow(), which enforces the same limit
    // at the input field itself (`input.maxLength`, kept in sync with
    // this constant by comment cross-reference, not a shared source - the
    // JS/C++ boundary has no shared header). This is also the hard limit:
    // sanitizeUserPresetFilename() below clamps to it regardless of what
    // reaches this function, so a name can never arrive here already too
    // long (e.g. from an older build's saved file, or a future caller
    // that skips the JS input's own maxLength).
    constexpr int userPresetMaxNameLength = 24;

    // Conservative filesystem-safe filename: keep alphanumerics/space/dash/
    // underscore, drop everything else (path separators, quotes, control
    // chars, etc.) so a user-typed preset name can never escape the presets
    // directory or collide with reserved characters on Windows/macOS: then
    // clamp to userPresetMaxNameLength above.
    inline juce::String sanitizeUserPresetFilename (const juce::String& name)
    {
        juce::String result;
        for (auto c : name)
        {
            if (juce::CharacterFunctions::isLetterOrDigit (c) || c == ' ' || c == '-' || c == '_')
                result += juce::String::charToString (c);
        }
        result = result.trim();
        if (result.length() > userPresetMaxNameLength)
            result = result.substring (0, userPresetMaxNameLength).trim();
        return result.isEmpty() ? "Untitled" : result;
    }

    inline juce::File getUserPresetFile (const juce::String& name)
    {
        return getUserPresetsDirectory().getChildFile (sanitizeUserPresetFilename (name) + ".uni76preset");
    }

    constexpr int userPresetSchemaVersion = 1;

    // One-time-per-scan self-healing migration: userPresetMaxNameLength
    // above is new (a real UX bug found live - a long preset name shifted
    // the header's prev/next arrows sideways every time it changed), so
    // any preset saved before this limit existed may still be sitting on
    // disk with a longer name than any future save could produce. Renamed
    // in place (never silently dropped) to the same clamped form
    // sanitizeUserPresetFilename() would now enforce, with a numeric
    // suffix on the rare collision (two overlong names that clamp to the
    // same prefix). Cheap: only runs the string work on files that are
    // already overlong, and the directory is already being iterated by
    // the caller.
    inline void renameOverlongUserPresetsIfNeeded (const juce::File& dir)
    {
        if (! dir.isDirectory())
            return;

        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*.uni76preset", juce::File::findFiles))
        {
            auto file = entry.getFile();
            const auto baseName = file.getFileNameWithoutExtension();
            if (baseName.length() <= userPresetMaxNameLength)
                continue;

            const auto clamped = sanitizeUserPresetFilename (baseName);
            auto target = dir.getChildFile (clamped + ".uni76preset");

            int suffix = 2;
            while (target.existsAsFile() && target != file)
            {
                const auto shortened = clamped.length() > userPresetMaxNameLength - 3
                                            ? clamped.substring (0, userPresetMaxNameLength - 3)
                                            : clamped;
                target = dir.getChildFile (shortened + " " + juce::String (suffix) + ".uni76preset");
                ++suffix;
            }

            file.moveFileTo (target);
        }
    }

    // Directory scan only - call on user gesture (PRESET menu open), never
    // at editor construction, so it never sits on the cold-open critical
    // path.
    inline juce::StringArray listUserPresetNames()
    {
        juce::StringArray names;
        auto dir = getUserPresetsDirectory();
        if (! dir.isDirectory())
            return names;

        renameOverlongUserPresetsIfNeeded (dir);

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
        static const char* paramNames[9] { "preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager", "imageTilt", "panRate" };
        for (int i = 0; i < 9; ++i)
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
        static const char* paramNames[9] { "preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager", "imageTilt", "panRate" };
        // A preset saved before PAN's RATE knob existed has no "panRate"
        // attribute at all - getDoubleAttribute's own default (0.0) would
        // silently read as the *slowest* possible rate, not "unchanged
        // from before this parameter existed". 35.303 is the exact
        // normalised position that reproduces PAN's original fixed
        // ~0.3Hz LFO speed (see ParameterLayout.cpp's own comment) - the
        // same fallback-to-current-default pattern
        // ModuleEnableState/imageTilt migrations already use.
        static const double paramDefaults[9] { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 35.303 };
        for (int i = 0; i < 9; ++i)
            data.values[(size_t) i] = (float) params->getDoubleAttribute (paramNames[i], paramDefaults[i]);

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
