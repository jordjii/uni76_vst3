#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <optional>

/*
    Serves the embedded diagnostic web UI (Resources/Web/*) to the plugin's
    WebBrowserComponent via JUCE's resource-provider mechanism.

    The HTML/CSS/JS is compiled into the binary as BinaryData by CMake's
    juce_add_binary_data() (see Source/Plugin/CMakeLists.txt) - there is no
    runtime dependency on the Resources/Web folder existing next to the
    plugin.
*/

namespace uni76::ui
{
    std::optional<juce::WebBrowserComponent::Resource> getWebResource (const juce::String& url);
}
