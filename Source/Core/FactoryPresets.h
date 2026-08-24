#pragma once

#include <array>

/*
    UNI 76 - RC1 factory preset bank.

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
    works. PITCH sits at 0 ST and TILT at 0/CENTER in every preset -
    both are deliberate creative choices with no natural "vintage
    character" default, so a generic starting point leaves them neutral
    rather than picking an arbitrary bias. No preset uses a 100% value
    anywhere - these are musical starting points, not stress-test
    settings (see docs/FULL_DSP_AUDIT.md for the plugin's own stress
    testing, which already covers 100%/extreme combinations separately).
*/

namespace uni76
{
    struct FactoryPreset
    {
        const char* name;

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

    inline constexpr std::array<FactoryPreset, 10> factoryPresets { {
        // Name              preamp  eq    sat   pitch  pan   verb  imager tilt
        { "Default",          0.0f, 50.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 0.0f },
        { "Warm Analog",     25.0f, 40.0f, 15.0f, 0.0f, 20.0f, 15.0f, 10.0f, 0.0f },
        { "Dark Vintage",    35.0f, 20.0f, 30.0f, 0.0f, 10.0f, 20.0f,  5.0f, 0.0f },
        { "Telephone Plate", 30.0f, 55.0f, 20.0f, 0.0f, 15.0f, 45.0f, 10.0f, 0.0f },
        { "Wide Vintage",    20.0f, 45.0f, 15.0f, 0.0f, 50.0f, 20.0f, 35.0f, 0.0f },
        { "Motion Space",    10.0f, 50.0f,  5.0f, 0.0f, 75.0f, 35.0f, 30.0f, 0.0f },
        { "Focused Stereo",  15.0f, 50.0f, 10.0f, 0.0f, 15.0f, 10.0f, 25.0f, 0.0f },
        { "Deep Plate",      15.0f, 45.0f, 10.0f, 0.0f, 20.0f, 80.0f, 15.0f, 0.0f },
        { "Hot Console",     55.0f, 60.0f, 45.0f, 0.0f, 15.0f, 10.0f, 15.0f, 0.0f },
        { "Clean Wide",       5.0f, 55.0f,  0.0f, 0.0f, 40.0f, 10.0f, 30.0f, 0.0f },
    } };
}
