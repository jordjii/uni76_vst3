#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "ChainOrder.h"
#include "../DSP/DelayCurves.h"

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
        std::array<float, 15> values {};       // ParamID::all order, real units
        std::array<bool, 8> moduleEnabled {};
        // Processing-chain order (drag-and-drop pedalboard round - see
        // Core/ChainOrder.h) - a preset that doesn't also capture its own
        // module order isn't fully reproducing "the sound" a user saved,
        // since reordering genuinely changes the processed audio. Defaults
        // to the current factory identity order (DELAY added 2026-09-14,
        // see docs/DSP_DELAY.md), NOT value-initialised zeros (which would
        // be role 0 repeated 8 times - not a valid permutation) - matters
        // for a preset saved before this field (or before DELAY) existed,
        // whose file has no ChainOrder node, or an old 7-token one, to
        // read back - see loadUserPreset()'s own migration for the latter.
        std::array<int, 8> chainOrder { 0, 1, 2, 3, 4, 7, 5, 6 };
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
        static const char* paramNames[15] {
            "preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager", "imageTilt", "panRate", "verbDrive",
            "delay", "delayFeedback", "delayDivision", "delayStereo", "delayPingPong"
        };
        for (int i = 0; i < 15; ++i)
            params->setAttribute (paramNames[i], (double) data.values[(size_t) i]);

        auto* modules = root.createNewChildElement ("ModulesEnabled");
        static const char* moduleNames[8] { "preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager", "delay" };
        for (int i = 0; i < 8; ++i)
            modules->setAttribute (moduleNames[i], data.moduleEnabled[(size_t) i]);

        // Same comma-joined-string convention ChainOrder::stateProperty
        // already uses for the plugin's own saved state.
        auto* chain = root.createNewChildElement ("ChainOrder");
        juce::StringArray orderTokens;
        for (int i = 0; i < 8; ++i)
            orderTokens.add (juce::String (data.chainOrder[(size_t) i]));
        chain->setAttribute ("order", orderTokens.joinIntoString (","));

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
        static const char* paramNames[15] {
            "preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager", "imageTilt", "panRate", "verbDrive",
            "delay", "delayFeedback", "delayDivision", "delayStereo", "delayPingPong"
        };
        // A preset saved before PAN's RATE / VERB's DRIVE knobs (or,
        // 2026-09-14, DELAY's own five parameters) existed has no such
        // attribute at all - getDoubleAttribute's own default (0.0) would
        // silently read panRate as division index 0 (1/128, absurdly
        // fast), not "the module's own default" (0.0 is the correct
        // fallback for verbDrive/delay/delayStereo/delayPingPong, though -
        // their own base values already *are* their identity/off state).
        // 68.182 is panRateDefaultNormalised*100 (PanoramaCurves.h); 30.0
        // is delayFeedback's own default (DelayCurves.h has no
        // "normalised" form of this one - 30% is the real unit directly,
        // same as every other plain percent parameter); 20.0 is
        // delayDivision's own default index (1, "1/8") expressed as the
        // 0..100 percent-of-choice-range getDoubleAttribute expects here
        // (1 of 4 steps = 25%... but see the note below - this reads back
        // through convertTo0to1 elsewhere, so the RAW value stored here
        // must be the real choice index, 1.0, not a percentage). Same
        // fallback-to-current-default pattern ModuleEnableState/imageTilt
        // migrations already use.
        static const double paramDefaults[15] {
            0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 68.182, 0.0,
            0.0, 30.0, (double) uni76::dsp::delayDefaultDivisionIndex, 0.0, 0.0
        };
        for (int i = 0; i < 15; ++i)
            data.values[(size_t) i] = (float) params->getDoubleAttribute (paramNames[i], paramDefaults[i]);

        static const char* moduleNames[8] { "preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager", "delay" };
        for (int i = 0; i < 8; ++i)
            data.moduleEnabled[(size_t) i] = modules->getBoolAttribute (moduleNames[i], true);

        // A preset saved before chain reordering existed has no
        // ChainOrder node at all - data.chainOrder already defaults to
        // the current factory identity order (see the struct's own
        // comment), so leave it untouched rather than reading back a
        // missing/invalid attribute. A preset saved before DELAY existed
        // has exactly 7 tokens - inserted safely before VERB, same
        // backward-compatibility rule PluginProcessor::setStateInformation()
        // applies to the plugin's own saved chain order. A hand-edited or
        // corrupt "order" string of either length is rejected the same
        // way ChainOrder itself rejects a bad saved state - fall back to
        // identity rather than apply a partial/duplicate permutation.
        if (auto* chain = xml->getChildByName ("ChainOrder"))
        {
            const auto tokens = juce::StringArray::fromTokens (chain->getStringAttribute ("order"), ",", "");
            if (tokens.size() == 8)
            {
                std::array<int, 8> candidate {};
                for (int i = 0; i < 8; ++i)
                    candidate[(size_t) i] = tokens[i].getIntValue();

                if (uni76::ChainOrder::isValidPermutation (candidate))
                    data.chainOrder = candidate;
            }
            else if (tokens.size() == 7)
            {
                std::array<int, 7> legacyCandidate {};
                for (int i = 0; i < 7; ++i)
                    legacyCandidate[(size_t) i] = tokens[i].getIntValue();

                data.chainOrder = uni76::ChainOrder::insertDelayIntoLegacyOrder (legacyCandidate);
            }
        }

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
