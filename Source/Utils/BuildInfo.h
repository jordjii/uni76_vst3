#pragma once

#include <juce_core/juce_core.h>

/*
    Small helper for reading the plugin identity JUCE generates from the
    juce_add_plugin() call (JucePlugin_* macros), so the rest of the code
    doesn't reference those macros directly. Used by the web UI bridge to
    tell the diagnostic frontend which version it's talking to.
*/

namespace uni76::BuildInfo
{
    inline juce::String getVersionString()  { return JucePlugin_VersionString; }
    inline juce::String getProductName()    { return JucePlugin_Name; }
    inline juce::String getCompanyName()    { return JucePlugin_Manufacturer; }
}
