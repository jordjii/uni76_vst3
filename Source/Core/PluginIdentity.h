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
    */
    inline constexpr int stateSchemaVersion = 1;

    /** Property name under which stateSchemaVersion is stored in the saved
        ValueTree, so setStateInformation can detect old presets.
    */
    inline constexpr const char* stateSchemaVersionProperty = "uni76StateSchemaVersion";
}
