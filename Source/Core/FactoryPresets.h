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

    Every preset keeps all 7 modules enabled - a preset is a musical
    starting *sound*, not a workflow shortcut for disabling modules;
    "off" character (e.g. no reverb) is expressed by that module's own
    parameter value (VERB=0%), matching how the product default already
    works. PITCH sits at 0 ST in every ordinary instrument preset (no
    natural "vintage character" default exists for a pitch shift), and
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
    };

    inline constexpr std::array<FactoryPreset, 32> factoryPresets { {
        // ---- GENERAL (unchanged from the RC1 round) --------------------
        // Name              Category                     preamp  eq    sat   pitch  pan   verb  imager tilt
        { "Default",          PresetCategory::general,        0.0f, 50.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 0.0f },
        { "Warm Analog",      PresetCategory::general,       25.0f, 40.0f, 15.0f, 0.0f, 20.0f, 15.0f, 10.0f, 0.0f },
        { "Dark Vintage",     PresetCategory::general,       35.0f, 20.0f, 30.0f, 0.0f, 10.0f, 20.0f,  5.0f, 0.0f },
        { "Telephone Plate",  PresetCategory::general,       30.0f, 55.0f, 20.0f, 0.0f, 15.0f, 45.0f, 10.0f, 0.0f },
        { "Wide Vintage",     PresetCategory::general,       20.0f, 45.0f, 15.0f, 0.0f, 50.0f, 20.0f, 35.0f, 0.0f },
        { "Motion Space",     PresetCategory::general,       10.0f, 50.0f,  5.0f, 0.0f, 75.0f, 35.0f, 30.0f, 0.0f },
        { "Focused Stereo",   PresetCategory::general,       15.0f, 50.0f, 10.0f, 0.0f, 15.0f, 10.0f, 25.0f, 0.0f },
        { "Deep Plate",       PresetCategory::general,       15.0f, 45.0f, 10.0f, 0.0f, 20.0f, 80.0f, 15.0f, 0.0f },
        { "Hot Console",      PresetCategory::general,       55.0f, 60.0f, 45.0f, 0.0f, 15.0f, 10.0f, 15.0f, 0.0f },
        { "Clean Wide",       PresetCategory::general,        5.0f, 55.0f,  0.0f, 0.0f, 40.0f, 10.0f, 30.0f, 0.0f },

        // ---- VOCAL ------------------------------------------------------
        // Gentle drive (vocals distort fast), EQ leaning AIR for presence,
        // modest width (vocals usually stay fairly centred), PLATE-range
        // VERB for the "sung into a room" cue.
        { "Warm Lead",        PresetCategory::vocal,          20.0f, 45.0f, 10.0f, 0.0f, 10.0f, 15.0f, 10.0f, 0.0f },
        { "Airy Lead",        PresetCategory::vocal,          10.0f, 70.0f,  5.0f, 0.0f, 10.0f, 20.0f, 15.0f, 0.0f },
        { "Vintage Vocal",    PresetCategory::vocal,          30.0f, 30.0f, 20.0f, 0.0f,  5.0f, 15.0f,  5.0f, 0.0f },
        { "Plate Vocal",      PresetCategory::vocal,          15.0f, 55.0f, 10.0f, 0.0f, 10.0f, 40.0f, 10.0f, 0.0f },
        { "Wide Backing",     PresetCategory::vocal,          10.0f, 50.0f,  5.0f, 0.0f, 45.0f, 25.0f, 30.0f, 0.0f },
        { "Lo-Fi Vocal",      PresetCategory::vocal,          40.0f, 15.0f, 35.0f, 0.0f,  0.0f, 10.0f,  0.0f, 0.0f },

        // ---- PIANO --------------------------------------------------------
        // Piano has a very wide natural frequency range - keep PREAMP/SAT
        // low (avoid muddying the bass end), use EQ/IMAGE/VERB to shape
        // width and space instead.
        { "Warm Upright",     PresetCategory::piano,          15.0f, 35.0f, 10.0f, 0.0f, 15.0f, 20.0f, 10.0f, 0.0f },
        { "Focused Grand",    PresetCategory::piano,          10.0f, 50.0f,  5.0f, 0.0f, 10.0f, 15.0f, 15.0f, 0.0f },
        { "Wide Grand",       PresetCategory::piano,          10.0f, 50.0f,  5.0f, 0.0f, 40.0f, 20.0f, 35.0f, 0.0f },
        { "Vintage Piano",    PresetCategory::piano,          25.0f, 25.0f, 15.0f, 0.0f, 15.0f, 15.0f,  5.0f, 0.0f },
        { "Deep Plate Piano", PresetCategory::piano,          10.0f, 45.0f,  5.0f, 0.0f, 20.0f, 55.0f, 15.0f, 0.0f },

        // ---- ACOUSTIC GUITAR -----------------------------------------------
        // Mid-driven instrument - moderate SAT for string "grit", IMAGE up
        // a bit more freely than piano/vocal since it sits well off-centre
        // in a mix.
        { "Warm Fingerstyle", PresetCategory::acousticGuitar, 20.0f, 40.0f, 15.0f, 0.0f, 15.0f, 15.0f, 15.0f, 0.0f },
        { "Bright Strum",     PresetCategory::acousticGuitar, 10.0f, 65.0f, 10.0f, 0.0f, 20.0f, 15.0f, 20.0f, 0.0f },
        { "Vintage Wood",     PresetCategory::acousticGuitar, 30.0f, 25.0f, 25.0f, 0.0f, 10.0f, 15.0f, 10.0f, 0.0f },
        { "Wide Acoustic",    PresetCategory::acousticGuitar, 15.0f, 50.0f, 10.0f, 0.0f, 45.0f, 20.0f, 35.0f, 0.0f },
        { "Plate Acoustic",   PresetCategory::acousticGuitar, 15.0f, 45.0f, 10.0f, 0.0f, 20.0f, 40.0f, 15.0f, 0.0f },

        // ---- ELECTRIC GUITAR -------------------------------------------
        // The one instrument family where real HEAT/DRIVE is idiomatic -
        // still nowhere near 100% (a genuinely overdriven amp is the
        // guitar's own job, not this insert's).
        { "Clean Console",    PresetCategory::electricGuitar, 20.0f, 45.0f, 10.0f, 0.0f, 10.0f, 10.0f, 10.0f, 0.0f },
        { "Warm Rhythm",      PresetCategory::electricGuitar, 30.0f, 35.0f, 25.0f, 0.0f, 15.0f, 10.0f, 10.0f, 0.0f },
        { "Vintage Lead",     PresetCategory::electricGuitar, 40.0f, 30.0f, 35.0f, 0.0f, 10.0f, 20.0f,  5.0f, 0.0f },
        { "Wide Clean",       PresetCategory::electricGuitar, 15.0f, 50.0f,  5.0f, 0.0f, 45.0f, 15.0f, 30.0f, 0.0f },
        { "Plate Lead",       PresetCategory::electricGuitar, 30.0f, 40.0f, 25.0f, 0.0f, 15.0f, 40.0f, 10.0f, 0.0f },
        { "Dark Rhythm",      PresetCategory::electricGuitar, 35.0f, 15.0f, 30.0f, 0.0f,  5.0f, 10.0f,  0.0f, 0.0f },
    } };
}
