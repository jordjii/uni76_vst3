#pragma once

#include <array>

/*
    Stable, centralised parameter identifiers for UNI 76.

    These strings are persisted in saved presets and referenced by DAW
    automation lanes. They must never be renamed once UNI 76 has shipped
    publicly - renaming an ID breaks every saved session and automation
    lane that references it.

    There were exactly 7 public parameters through the plugin's initial
    development (see CLAUDE.md's history). `imageTilt` is a deliberate,
    explicit exception to that "one knob per module" rule, added once
    IMAGE itself gained real DSP: IMAGE now has two independent axes -
    `imager` (frequency-dependent width/imaging amount) and `imageTilt`
    (a static stereo left/right balance-tilt on top of that same image) -
    see docs/DSP_IMAGE.md. Low Cut / High Cut remain internal to PREAMP,
    not separate automatable parameters.

    `panRate` (9th parameter, PAN's nested RATE knob - see docs/DSP_PAN.md's
    "Motion rate" section) is the second such deliberate exception: PAN's
    motion LFO speed was a fixed constant (~0.3Hz) until this round, and is
    now a real, automatable, user-adjustable axis alongside `panorama`'s
    existing width/motion-depth control - the same "genuinely independent
    second axis on one module, not one knob controlling two things" reasoning
    IMAGE's `imageTilt` addition already established as this project's own
    precedent for growing past 8 parameters.
*/

namespace uni76::ParamID
{
    inline constexpr const char* preamp     = "preamp";
    inline constexpr const char* eq         = "eq";
    inline constexpr const char* saturation = "saturation";
    inline constexpr const char* pitch      = "pitch";
    inline constexpr const char* panorama   = "panorama";
    inline constexpr const char* reverb     = "reverb";
    inline constexpr const char* imager     = "imager";
    inline constexpr const char* imageTilt  = "imageTilt";
    inline constexpr const char* panRate    = "panRate";

    /** Version tag passed to juce::ParameterID for every parameter below.
        JUCE mixes this into the VST3 parameter hash; bump it only if a
        parameter's meaning changes in a way that should be treated as a
        new automation target by hosts.
    */
    inline constexpr int parameterVersionHint = 1;

    /** All parameter IDs, for iteration (tests, UI wiring, etc). */
    inline constexpr std::array<const char*, 9> all
    {
        preamp, eq, saturation, pitch, panorama, reverb, imager, imageTilt, panRate
    };
}
