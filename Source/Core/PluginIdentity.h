#pragma once

/*
    Centralised, non-DSP identity constants for UNI 76.

    Company/bundle/plugin-code identity lives in cmake/PluginIdentity.cmake
    (it must be visible to CMake). This header holds the C++-only constants
    that don't need to be known at configure time, such as the preset/state
    schema version used to migrate saved sessions in future releases.
*/

namespace uni76
{
    /** Bumped whenever the shape of the saved APVTS state changes in a way
        that requires migration code in PluginProcessor::setStateInformation.

        v1: the 7 APVTS parameters only.
        v2: added the 7 module-enabled flags (Core/ModuleEnableState.h) as
            plain properties alongside the parameters. A v1 state simply
            won't have these properties, and setStateInformation() already
            treats a missing flag as enabled=true, so no dedicated
            migration branch was needed for this bump - the version number
            is still incremented so the saved format is self-documenting.
        v3: `pitch` changed from a 0..100% AudioParameterFloat (no DSP
            behind it) to a discrete -12..+12 semitone AudioParameterInt
            (see docs/DSP_PITCH.md). A pre-v3 state's saved `pitch` value
            is a raw 0..100 number that never had any semitone meaning -
            setStateInformation() explicitly forces `pitch` to its new
            default (0 ST) whenever loadedSchemaVersion < 3, rather than
            letting APVTS reinterpret that old raw number as a semitone
            offset (which would silently clamp/misread it).
    */
    inline constexpr int stateSchemaVersion = 3;

    /** Property name under which stateSchemaVersion is stored in the saved
        ValueTree, so setStateInformation can detect old presets.
    */
    inline constexpr const char* stateSchemaVersionProperty = "uni76StateSchemaVersion";

    /** The schema version at which `pitch` became a discrete -12..+12
        AudioParameterInt (see stateSchemaVersion's v3 entry above). Fixed
        at 3 regardless of any later bump to stateSchemaVersion - a future
        unrelated migration must not re-trigger the pitch-value-stripping
        branch in setStateInformation() for states that are already v3+.
    */
    inline constexpr int pitchDiscreteSchemaVersion = 3;
}
