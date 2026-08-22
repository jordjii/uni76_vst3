#pragma once

#include <array>

/*
    Stable, centralised parameter identifiers for UNI 76.

    These strings are persisted in saved presets and referenced by DAW
    automation lanes. They must never be renamed once UNI 76 has shipped
    publicly - renaming an ID breaks every saved session and automation
    lane that references it.

    There are exactly 7 public parameters at this stage. Low Cut / High Cut
    are deliberately NOT here: they will become internal implementation
    details of the future PREAMP DSP module, not separate automatable
    parameters.
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

    /** Version tag passed to juce::ParameterID for every parameter below.
        JUCE mixes this into the VST3 parameter hash; bump it only if a
        parameter's meaning changes in a way that should be treated as a
        new automation target by hosts.
    */
    inline constexpr int parameterVersionHint = 1;

    /** All parameter IDs, for iteration (tests, UI wiring, etc). */
    inline constexpr std::array<const char*, 7> all
    {
        preamp, eq, saturation, pitch, panorama, reverb, imager
    };
}
