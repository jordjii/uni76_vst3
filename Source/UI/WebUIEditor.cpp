#include "WebUIEditor.h"
#include "WebResourceProvider.h"
#include "Plugin/PluginProcessor.h"
#include "Parameters/ParameterIDs.h"
#include "Core/MeterEnvelope.h"
#include "Core/FactoryPresets.h"

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
        UNI76AudioProcessor& processor,
        UNI76AudioProcessorEditor& editor)
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
            // RC1 factory presets (see Core/FactoryPresets.h) - a preset is
            // just a named set of values for the existing 8 parameters +
            // the 7 module-enable flags, applied through the exact same
            // setValueNotifyingHost()/ModuleEnableState::setEnabled() paths
            // a user's own gesture already exercises. No new saved-state
            // format, no new parameter.
            .withNativeFunction ("uni76GetFactoryPresetNames",
                [] (const juce::Array<juce::var>&, Completion complete)
                {
                    juce::Array<juce::var> names;
                    for (auto& preset : uni76::factoryPresets)
                        names.add (juce::String (preset.name));

                    complete (juce::var (names));
                })
            .withNativeFunction ("uni76LoadFactoryPreset",
                [&processor] (const juce::Array<juce::var>& args, Completion complete)
                {
                    if (args.size() >= 1)
                    {
                        const auto index = (int) args[0];
                        if (index >= 0 && index < (int) uni76::factoryPresets.size())
                        {
                            const auto& preset = uni76::factoryPresets[(size_t) index];
                            auto& apvts = processor.getValueTreeState();

                            const float rawValues[8] {
                                preset.preamp, preset.eq, preset.saturation, preset.pitch,
                                preset.panorama, preset.reverb, preset.imager, preset.imageTilt
                            };

                            for (size_t i = 0; i < uni76::ParamID::all.size(); ++i)
                            {
                                if (auto* param = apvts.getParameter (uni76::ParamID::all[i]))
                                    param->setValueNotifyingHost (param->convertTo0to1 (rawValues[i]));
                            }

                            // Every factory preset ships with all modules
                            // enabled (see Core/FactoryPresets.h's own
                            // rationale) - a preset is a starting sound,
                            // not a workflow shortcut for muting modules.
                            for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                                processor.getModuleEnableState().setEnabled (i, true);
                        }
                    }

                    complete (juce::var());
                })
            // RC1 A/B (see WebUIEditor.h's ABSnapshot comment) - session-
            // local only, not persisted.
            .withNativeFunction ("uni76ToggleAB",
                [&editor] (const juce::Array<juce::var>&, Completion complete)
                {
                    complete (juce::var (editor.toggleAB()));
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
                                    controlParameterIndexReceiver, p, *this)),
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

    // A/B starts with both slots identical to whatever the processor
    // already holds (fresh defaults, or a just-loaded state) - the first
    // toggle simply flips to an editable copy of the same sound, not to
    // silence/defaults.
    abSlotA = abSlotB = captureSnapshot();
}

UNI76AudioProcessorEditor::~UNI76AudioProcessorEditor() = default;

UNI76AudioProcessorEditor::ABSnapshot UNI76AudioProcessorEditor::captureSnapshot() const
{
    ABSnapshot snapshot;
    auto& apvts = processor.getValueTreeState();

    for (size_t i = 0; i < uni76::ParamID::all.size(); ++i)
        if (auto* param = apvts.getParameter (uni76::ParamID::all[i]))
            snapshot.values[i] = param->getValue();

    for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
        snapshot.moduleEnabled[(size_t) i] = processor.getModuleEnableState().isEnabled (i);

    return snapshot;
}

void UNI76AudioProcessorEditor::applySnapshot (const ABSnapshot& snapshot)
{
    auto& apvts = processor.getValueTreeState();

    for (size_t i = 0; i < uni76::ParamID::all.size(); ++i)
        if (auto* param = apvts.getParameter (uni76::ParamID::all[i]))
            param->setValueNotifyingHost (snapshot.values[i]);

    for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
        processor.getModuleEnableState().setEnabled (i, snapshot.moduleEnabled[(size_t) i]);
}

juce::String UNI76AudioProcessorEditor::toggleAB()
{
    // Capture whatever the user has tweaked since the last toggle into
    // the slot that's *currently* active, then switch to the other one -
    // so neither slot silently loses in-progress edits.
    (abActiveIsA ? abSlotA : abSlotB) = captureSnapshot();
    abActiveIsA = ! abActiveIsA;
    applySnapshot (abActiveIsA ? abSlotA : abSlotB);

    return abActiveIsA ? "A" : "B";
}

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
