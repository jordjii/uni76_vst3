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
    */
    inline constexpr int stateSchemaVersion = 2;

    /** Property name under which stateSchemaVersion is stored in the saved
        ValueTree, so setStateInformation can detect old presets.
    */
    inline constexpr const char* stateSchemaVersionProperty = "uni76StateSchemaVersion";
}
