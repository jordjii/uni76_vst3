#include "WebUIEditor.h"
#include "WebResourceProvider.h"
#include "Plugin/PluginProcessor.h"
#include "Parameters/ParameterIDs.h"

namespace
{
    // WebView2 must never write its user-data cache into the plugin's own
    // install directory (that location may not be writable, and multiple
    // plugin copies/instances would collide). Use the per-user app-data
    // area instead.
    juce::File getWebView2UserDataFolder()
    {
        return juce::File::getSpecialLocation (juce::File::SpecialLocationType::userApplicationDataDirectory)
                   .getChildFile ("Nostalgia Audio")
                   .getChildFile ("UNI 76")
                   .getChildFile ("WebView2");
    }

    juce::WebBrowserComponent::Options makeWebViewOptions (
        juce::WebSliderRelay& preamp,
        juce::WebSliderRelay& eq,
        juce::WebSliderRelay& saturation,
        juce::WebSliderRelay& pitch,
        juce::WebSliderRelay& panorama,
        juce::WebSliderRelay& reverb,
        juce::WebSliderRelay& imager,
        juce::WebControlParameterIndexReceiver& indexReceiver)
    {
        using Options = juce::WebBrowserComponent::Options;

        // Requesting the webview2 backend is safe on every platform: JUCE
        // silently falls back to the platform default (WKWebView on macOS)
        // when webview2 isn't applicable. WebView2Loader is statically
        // linked (see Source/Plugin/CMakeLists.txt), so there is no loader
        // DLL to locate at runtime - only the user-data folder needs to be
        // redirected away from the plugin's own (possibly read-only) install
        // directory.
        return Options{}
            .withBackend (Options::Backend::webview2)
            .withWinWebView2Options (Options::WinWebView2{}
                                          .withUserDataFolder (getWebView2UserDataFolder())
                                          .withBackgroundColour (juce::Colour { 0xff141414 }))
            .withNativeIntegrationEnabled()
            .withOptionsFrom (preamp)
            .withOptionsFrom (eq)
            .withOptionsFrom (saturation)
            .withOptionsFrom (pitch)
            .withOptionsFrom (panorama)
            .withOptionsFrom (reverb)
            .withOptionsFrom (imager)
            .withOptionsFrom (indexReceiver)
            .withResourceProvider (&uni76::ui::getWebResource);
    }
}

bool UNI76AudioProcessorEditor::SinglePageBrowser::pageAboutToLoad (const juce::String& newURL)
{
    // Only our own embedded resource root may ever be loaded - block any
    // attempt to navigate the frontend to a third-party site.
    return newURL == juce::WebBrowserComponent::getResourceProviderRoot();
}

namespace
{
    // Message-thread-only envelope follower: fast attack, slow release, so
    // the meter reads as a smooth analogue needle rather than a flickering
    // per-block value. Tuned for a 30 Hz timer (see startTimerHz below).
    constexpr float meterAttackCoeff  = 0.6f;
    constexpr float meterReleaseCoeff = 0.08f;

    float applyMeterEnvelope (float previous, float target) noexcept
    {
        const auto coeff = target > previous ? meterAttackCoeff : meterReleaseCoeff;
        return previous + (target - previous) * coeff;
    }
}

UNI76AudioProcessorEditor::UNI76AudioProcessorEditor (UNI76AudioProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      webView (makeWebViewOptions (preampRelay, eqRelay, saturationRelay, pitchRelay,
                                    panoramaRelay, reverbRelay, imagerRelay,
                                    controlParameterIndexReceiver)),
      preampAttachment     (*processor.getValueTreeState().getParameter (uni76::ParamID::preamp),
                             preampRelay, processor.getValueTreeState().undoManager),
      eqAttachment         (*processor.getValueTreeState().getParameter (uni76::ParamID::eq),
                             eqRelay, processor.getValueTreeState().undoManager),
      saturationAttachment (*processor.getValueTreeState().getParameter (uni76::ParamID::saturation),
                             saturationRelay, processor.getValueTreeState().undoManager),
      pitchAttachment      (*processor.getValueTreeState().getParameter (uni76::ParamID::pitch),
                             pitchRelay, processor.getValueTreeState().undoManager),
      panoramaAttachment   (*processor.getValueTreeState().getParameter (uni76::ParamID::panorama),
                             panoramaRelay, processor.getValueTreeState().undoManager),
      reverbAttachment     (*processor.getValueTreeState().getParameter (uni76::ParamID::reverb),
                             reverbRelay, processor.getValueTreeState().undoManager),
      imagerAttachment     (*processor.getValueTreeState().getParameter (uni76::ParamID::imager),
                             imagerRelay, processor.getValueTreeState().undoManager)
{
    addAndMakeVisible (webView);
    webView.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());

    // Fixed 3:2 aspect ratio across the whole resize range: 600x400 (min),
    // 960x640 (default), 1350x900 (max). The UI itself is laid out in
    // relative CSS units (see Resources/Web/tokens.css and responsive.css),
    // so it reflows to fill whatever size the host allows within these
    // limits rather than being pinned to a fixed pixel canvas.
    setResizable (true, true);
    setResizeLimits (600, 400, 1350, 900);

    if (auto* editorConstrainer = getConstrainer())
        editorConstrainer->setFixedAspectRatio (3.0 / 2.0);

    setSize (960, 640);

    // Meter telemetry only - reads the processor's lock-free LevelMeters
    // and forwards a smoothed value to the WebView. Purely a UI concern;
    // the audio thread never waits on or calls into this.
    startTimerHz (30);
}

UNI76AudioProcessorEditor::~UNI76AudioProcessorEditor() = default;

void UNI76AudioProcessorEditor::resized()
{
    webView.setBounds (getLocalBounds());
}

int UNI76AudioProcessorEditor::getControlParameterIndex (Component&)
{
    return controlParameterIndexReceiver.getControlParameterIndex();
}

void UNI76AudioProcessorEditor::timerCallback()
{
    const auto inputPeak  = processor.getInputLevelMeter().readAndResetPeak();
    const auto outputPeak = processor.getOutputLevelMeter().readAndResetPeak();

    inputMeterEnvelope  = applyMeterEnvelope (inputMeterEnvelope, inputPeak);
    outputMeterEnvelope = applyMeterEnvelope (outputMeterEnvelope, outputPeak);

    auto* payload = new juce::DynamicObject();
    payload->setProperty ("input", inputMeterEnvelope);
    payload->setProperty ("output", outputMeterEnvelope);

    webView.emitEventIfBrowserIsVisible ("meterLevels", juce::var (payload));
}
