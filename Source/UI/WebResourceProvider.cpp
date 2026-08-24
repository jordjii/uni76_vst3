#include "WebResourceProvider.h"
#include "BinaryData.h"

#include <cstring>
#include <unordered_map>

namespace uni76::ui
{
    namespace
    {
        struct Entry
        {
            const char* data;
            int size;
            const char* mimeType;
        };

        const std::unordered_map<juce::String, Entry>& getResourceTable()
        {
            static const std::unordered_map<juce::String, Entry> table
            {
                { "index.html",      { BinaryData::index_html,      BinaryData::index_htmlSize,      "text/html" } },

                { "tokens.css",      { BinaryData::tokens_css,      BinaryData::tokens_cssSize,      "text/css" } },
                { "reset.css",       { BinaryData::reset_css,       BinaryData::reset_cssSize,       "text/css" } },
                { "shell.css",       { BinaryData::shell_css,       BinaryData::shell_cssSize,       "text/css" } },
                { "header.css",      { BinaryData::header_css,      BinaryData::header_cssSize,      "text/css" } },
                { "modules.css",     { BinaryData::modules_css,     BinaryData::modules_cssSize,     "text/css" } },
                { "knobs.css",       { BinaryData::knobs_css,       BinaryData::knobs_cssSize,       "text/css" } },
                { "scales.css",      { BinaryData::scales_css,      BinaryData::scales_cssSize,      "text/css" } },
                { "meters.css",      { BinaryData::meters_css,      BinaryData::meters_cssSize,      "text/css" } },
                { "responsive.css",  { BinaryData::responsive_css,  BinaryData::responsive_cssSize,  "text/css" } },

                { "app.js",          { BinaryData::app_js,          BinaryData::app_jsSize,          "text/javascript" } },
                { "knob.js",         { BinaryData::knob_js,         BinaryData::knob_jsSize,         "text/javascript" } },
                { "field_pad.js",    { BinaryData::field_pad_js,    BinaryData::field_pad_jsSize,    "text/javascript" } },
                { "aux_visuals.js",  { BinaryData::aux_visuals_js,  BinaryData::aux_visuals_jsSize,  "text/javascript" } },
                { "meters.js",       { BinaryData::meters_js,       BinaryData::meters_jsSize,       "text/javascript" } },
                { "module_power.js", { BinaryData::module_power_js, BinaryData::module_power_jsSize, "text/javascript" } },
                { "header_controls.js", { BinaryData::header_controls_js, BinaryData::header_controls_jsSize, "text/javascript" } },
                { "juce_webview.js", { BinaryData::juce_webview_js, BinaryData::juce_webview_jsSize, "text/javascript" } },
            };

            return table;
        }
    }

    std::optional<juce::WebBrowserComponent::Resource> getWebResource (const juce::String& url)
    {
        const auto path = url == "/" ? juce::String { "index.html" }
                                      : url.fromFirstOccurrenceOf ("/", false, false);

        const auto& table = getResourceTable();
        const auto it = table.find (path);

        if (it == table.end())
            return std::nullopt;

        const auto& entry = it->second;

        std::vector<std::byte> bytes (static_cast<size_t> (entry.size));
        std::memcpy (bytes.data(), entry.data, bytes.size());

        return juce::WebBrowserComponent::Resource { std::move (bytes), juce::String { entry.mimeType } };
    }
}
