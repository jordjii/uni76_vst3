#include "WebUIEditor.h"
#include "WebResourceProvider.h"
#include "Plugin/PluginProcessor.h"
#include "Parameters/ParameterIDs.h"
#include "Core/MeterEnvelope.h"

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
        juce::WebSliderRelay& imageTilt,
        juce::WebControlParameterIndexReceiver& indexReceiver,
        UNI76AudioProcessor& processor)
    {
        using Options = juce::WebBrowserComponent::Options;
        using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;

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
            .withOptionsFrom (imageTilt)
            .withOptionsFrom (indexReceiver)
            // The 7 module-enabled flags are persistent but NOT DAW
            // automation parameters (see Core/ModuleEnableState.h), so
            // they don't get a WebToggleRelay - this small pair of native
            // functions is the whole bridge: the frontend calls the getter
            // once at startup to sync its resting-enabled HTML with
            // whatever was actually loaded, and calls the setter whenever
            // the user clicks a module's power toggle.
            .withNativeFunction ("uni76SetModuleEnabled",
                [&processor] (const juce::Array<juce::var>& args, Completion complete)
                {
                    if (args.size() >= 2)
                        processor.getModuleEnableState().setEnabled ((int) args[0], (bool) args[1]);

                    complete (juce::var());
                })
            .withNativeFunction ("uni76GetModuleEnabledStates",
                [&processor] (const juce::Array<juce::var>&, Completion complete)
                {
                    juce::Array<juce::var> states;
                    for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                        states.add (processor.getModuleEnableState().isEnabled (i));

                    complete (juce::var (states));
                })
            .withResourceProvider (&uni76::ui::getWebResource);
    }
}

bool UNI76AudioProcessorEditor::SinglePageBrowser::pageAboutToLoad (const juce::String& newURL)
{
    // Only our own embedded resource root may ever be loaded - block any
    // attempt to navigate the frontend to a third-party site.
    return newURL == juce::WebBrowserComponent::getResourceProviderRoot();
}

UNI76AudioProcessorEditor::UNI76AudioProcessorEditor (UNI76AudioProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      webView (makeWebViewOptions (preampRelay, eqRelay, saturationRelay, pitchRelay,
                                    panoramaRelay, reverbRelay, imagerRelay, imageTiltRelay,
                                    controlParameterIndexReceiver, p)),
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
                             imagerRelay, processor.getValueTreeState().undoManager),
      imageTiltAttachment  (*processor.getValueTreeState().getParameter (uni76::ParamID::imageTilt),
                             imageTiltRelay, processor.getValueTreeState().undoManager)
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

    inputMeterEnvelope  = uni76::applyMeterEnvelope (inputMeterEnvelope, inputPeak);
    outputMeterEnvelope = uni76::applyMeterEnvelope (outputMeterEnvelope, outputPeak);

    auto* payload = new juce::DynamicObject();
    payload->setProperty ("input", inputMeterEnvelope);
    payload->setProperty ("output", outputMeterEnvelope);

    webView.emitEventIfBrowserIsVisible ("meterLevels", juce::var (payload));
}
