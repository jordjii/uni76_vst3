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
                { "style.css",       { BinaryData::style_css,       BinaryData::style_cssSize,       "text/css" } },
                { "app.js",          { BinaryData::app_js,          BinaryData::app_jsSize,          "text/javascript" } },
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
