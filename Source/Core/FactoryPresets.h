#pragma once

#include <array>

/*
    UNI 76 - factory preset bank.

    Deliberately NOT a new saved-state format and NOT a new APVTS
    parameter: a preset is just a named set of *values* for the existing
    8 stable parameters (see Parameters/ParameterIDs.h - IDs are never
    renamed here) plus the 7 module-enabled flags (ModuleEnableState.h).
    Selecting a preset calls the exact same `setValueNotifyingHost()` /
    `ModuleEnableState::setEnabled()` paths a user turning a knob or
    clicking a power button already exercises - so it needs no new
    migration logic and is automatically captured by the existing
    getStateInformation()/setStateInformation() the next time the host
    saves, with no changes to that mechanism at all.

    Values are the parameters' own real (denormalised) units - the same
    units ParameterLayout.cpp declares each range in - not normalised
    0..1, so this table reads directly against docs/DSP_*.md's own
    percent/semitone/tilt language. Source/UI/WebUIEditor.cpp converts
    each value with that parameter's own `convertTo0to1()` before calling
    setValueNotifyingHost().

    `modulesEnabled` (live-testing follow-up round - not every module
    used to be enabled in every preset) - each preset now enables only
    the modules that genuinely contribute to its own sound, following
    two explicit rules:
      - PREAMP/SAT/PAN/VERB/IMAGE are enabled exactly when that module's
        own value for this preset is non-zero - a module sitting at 0%
        isn't "used" by this preset's own recipe.
      - EQ is enabled *only* on presets whose whole point is the always-
        on band-limited "telephone"/lo-fi character it now has (see
        docs/DSP_EQ.md's "Redesign" section) - "Telephone Plate" and
        "Lo-Fi Vocal" - never as a blanket default, since EQ's own centre
        position is no longer a transparent resting state.
      - PITCH is disabled everywhere: every ordinary instrument preset
        keeps it at 0 ST (no natural "vintage character" default exists
        for a pitch shift), so it never actually contributes anything a
        user hasn't dialled in themselves.
    "Default" therefore has every module disabled - a genuine, literal
    pass-through starting point, not just parameter values that happen to
    be near-identity.

    TILT stays at 0/CENTER unless a preset has a genuine artistic reason
    to lean the stereo image (documented per-preset below where used). No
    preset reaches 100% on any control - these are musical starting
    points, not the extreme-matrix stress combinations
    docs/FULL_DSP_AUDIT.md's own audit already covers separately.
*/

namespace uni76
{
    enum class PresetCategory
    {
        general,
        vocal,
        piano,
        acousticGuitar,
        electricGuitar
    };

    inline const char* presetCategoryName (PresetCategory category) noexcept
    {
        switch (category)
        {
            case PresetCategory::general:        return "GENERAL";
            case PresetCategory::vocal:           return "VOCAL";
            case PresetCategory::piano:           return "PIANO";
            case PresetCategory::acousticGuitar:  return "ACOUSTIC GUITAR";
            case PresetCategory::electricGuitar:  return "ELECTRIC GUITAR";
        }
        return "GENERAL";
    }

    struct FactoryPreset
    {
        const char* name;
        PresetCategory category;

        // Real units, in ParamID::all order: preamp%, eq%, saturation%,
        // pitchST, panorama%, reverb%, imager%, imageTilt.
        float preamp;
        float eq;
        float saturation;
        float pitch;
        float panorama;
        float reverb;
        float imager;
        float imageTilt;

        // Module-enabled flags, in ModuleEnableState.h's own order:
        // preamp, eq, saturation, pitch, panorama, reverb, imager.
        std::array<bool, 7> modulesEnabled;
    };

    inline constexpr std::array<FactoryPreset, 32> factoryPresets { {
        // ---- GENERAL ------------------------------------------------------
        // Name              Category                     preamp  eq    sat   pitch  pan   verb  imager tilt   { preamp, eq,    sat,   pitch, pan,   verb,  imager }
        { "Default",          PresetCategory::general,        0.0f, 50.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 0.0f, { false, false, false, false, false, false, false } },
        { "Warm Analog",      PresetCategory::general,       25.0f, 40.0f, 15.0f, 0.0f, 20.0f, 15.0f, 10.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Dark Vintage",     PresetCategory::general,       35.0f, 20.0f, 30.0f, 0.0f, 10.0f, 20.0f,  5.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Telephone Plate",  PresetCategory::general,       30.0f, 55.0f, 20.0f, 0.0f, 15.0f, 45.0f, 10.0f, 0.0f, { true,  true,  true,  false, true,  true,  true  } },
        { "Wide Vintage",     PresetCategory::general,       20.0f, 45.0f, 15.0f, 0.0f, 50.0f, 20.0f, 35.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Motion Space",     PresetCategory::general,       10.0f, 50.0f,  5.0f, 0.0f, 75.0f, 35.0f, 30.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Focused Stereo",   PresetCategory::general,       15.0f, 50.0f, 10.0f, 0.0f, 15.0f, 10.0f, 25.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Deep Plate",       PresetCategory::general,       15.0f, 45.0f, 10.0f, 0.0f, 20.0f, 80.0f, 15.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Hot Console",      PresetCategory::general,       55.0f, 60.0f, 45.0f, 0.0f, 15.0f, 10.0f, 15.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Clean Wide",       PresetCategory::general,        5.0f, 55.0f,  0.0f, 0.0f, 40.0f, 10.0f, 30.0f, 0.0f, { true,  false, false, false, true,  true,  true  } },

        // ---- VOCAL ------------------------------------------------------
        // Gentle drive (vocals distort fast), EQ leaning AIR for presence,
        // modest width (vocals usually stay fairly centred), PLATE-range
        // VERB for the "sung into a room" cue.
        { "Warm Lead",        PresetCategory::vocal,          20.0f, 45.0f, 10.0f, 0.0f, 10.0f, 15.0f, 10.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Airy Lead",        PresetCategory::vocal,          10.0f, 70.0f,  5.0f, 0.0f, 10.0f, 20.0f, 15.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Vintage Vocal",    PresetCategory::vocal,          30.0f, 30.0f, 20.0f, 0.0f,  5.0f, 15.0f,  5.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Plate Vocal",      PresetCategory::vocal,          15.0f, 55.0f, 10.0f, 0.0f, 10.0f, 40.0f, 10.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Wide Backing",     PresetCategory::vocal,          10.0f, 50.0f,  5.0f, 0.0f, 45.0f, 25.0f, 30.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Lo-Fi Vocal",      PresetCategory::vocal,          40.0f, 15.0f, 35.0f, 0.0f,  0.0f, 10.0f,  0.0f, 0.0f, { true,  true,  true,  false, false, true,  false } },

        // ---- PIANO --------------------------------------------------------
        // Piano has a very wide natural frequency range - keep PREAMP/SAT
        // low (avoid muddying the bass end), use IMAGE/VERB to shape width
        // and space instead. EQ stays off - its always-on telephone-band
        // character is never appropriate for piano's own wide range.
        { "Warm Upright",     PresetCategory::piano,          15.0f, 35.0f, 10.0f, 0.0f, 15.0f, 20.0f, 10.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Focused Grand",    PresetCategory::piano,          10.0f, 50.0f,  5.0f, 0.0f, 10.0f, 15.0f, 15.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Wide Grand",       PresetCategory::piano,          10.0f, 50.0f,  5.0f, 0.0f, 40.0f, 20.0f, 35.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Vintage Piano",    PresetCategory::piano,          25.0f, 25.0f, 15.0f, 0.0f, 15.0f, 15.0f,  5.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Deep Plate Piano", PresetCategory::piano,          10.0f, 45.0f,  5.0f, 0.0f, 20.0f, 55.0f, 15.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },

        // ---- ACOUSTIC GUITAR -----------------------------------------------
        // Mid-driven instrument - moderate SAT for string "grit", IMAGE up
        // a bit more freely than piano/vocal since it sits well off-centre
        // in a mix. EQ stays off here too - none of these are meant to
        // sound band-limited/telephone.
        { "Warm Fingerstyle", PresetCategory::acousticGuitar, 20.0f, 40.0f, 15.0f, 0.0f, 15.0f, 15.0f, 15.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Bright Strum",     PresetCategory::acousticGuitar, 10.0f, 65.0f, 10.0f, 0.0f, 20.0f, 15.0f, 20.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Vintage Wood",     PresetCategory::acousticGuitar, 30.0f, 25.0f, 25.0f, 0.0f, 10.0f, 15.0f, 10.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Wide Acoustic",    PresetCategory::acousticGuitar, 15.0f, 50.0f, 10.0f, 0.0f, 45.0f, 20.0f, 35.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Plate Acoustic",   PresetCategory::acousticGuitar, 15.0f, 45.0f, 10.0f, 0.0f, 20.0f, 40.0f, 15.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },

        // ---- ELECTRIC GUITAR -------------------------------------------
        // The one instrument family where real HEAT/DRIVE is idiomatic -
        // still nowhere near 100% (a genuinely overdriven amp is the
        // guitar's own job, not this insert's). EQ stays off - none of
        // these lean into a telephone/lo-fi character either.
        { "Clean Console",    PresetCategory::electricGuitar, 20.0f, 45.0f, 10.0f, 0.0f, 10.0f, 10.0f, 10.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Warm Rhythm",      PresetCategory::electricGuitar, 30.0f, 35.0f, 25.0f, 0.0f, 15.0f, 10.0f, 10.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Vintage Lead",     PresetCategory::electricGuitar, 40.0f, 30.0f, 35.0f, 0.0f, 10.0f, 20.0f,  5.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Wide Clean",       PresetCategory::electricGuitar, 15.0f, 50.0f,  5.0f, 0.0f, 45.0f, 15.0f, 30.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Plate Lead",       PresetCategory::electricGuitar, 30.0f, 40.0f, 25.0f, 0.0f, 15.0f, 40.0f, 10.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Dark Rhythm",      PresetCategory::electricGuitar, 35.0f, 15.0f, 30.0f, 0.0f,  5.0f, 10.0f,  0.0f, 0.0f, { true,  false, true,  false, true,  true,  false } },
    } };
}
