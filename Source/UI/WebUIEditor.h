#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <array>

class UNI76AudioProcessor;

/*
    UNI 76's WebView editor.

    Renders the embedded HTML/CSS/JS UI (Resources/Web) inside a
    WebBrowserComponent and binds each of the 7 parameters to it via JUCE's
    WebSliderRelay / WebSliderParameterAttachment bridge, so that:
      - moving an HTML control updates the JUCE parameter (-> DAW automation
        and state).
      - automation or state changes from the DAW/host update the HTML
        control.

    Resizable within a fixed 3:2 aspect ratio - see the constructor.

    Also runs a message-thread Timer that polls the processor's lock-free
    INPUT/OUTPUT LevelMeters, applies attack/release envelope smoothing,
    and pushes the result to the WebView as a "meterLevels" JS event. The
    audio thread itself never touches the WebView - see
    Source/Core/LevelMeter.h and PluginProcessor::processBlock().
*/
class UNI76AudioProcessorEditor final : public juce::AudioProcessorEditor,
                                         private juce::Timer
{
public:
    explicit UNI76AudioProcessorEditor (UNI76AudioProcessor&);
    ~UNI76AudioProcessorEditor() override;

    void resized() override;

    int getControlParameterIndex (Component&) override;

    // ---- A/B (RC1 minimal implementation - see docs/FULL_DSP_AUDIT.md's
    // RC1 report) - two in-memory snapshots of the 8 parameters + 7
    // module-enable flags, live only for this editor's lifetime. Not part
    // of getStateInformation()/setStateInformation() and not persisted -
    // a session-local comparison aid, the same way a DAW's own undo
    // history isn't saved into the project either. Toggling captures the
    // currently-active slot's live values (so in-progress edits aren't
    // lost) before applying the other slot. Public (not just called from
    // this class) because the native-function bridge lambda in
    // WebUIEditor.cpp's makeWebViewOptions() is a free function, not a
    // member - the same reason getControlParameterIndex() above is a
    // public override rather than private.
    juce::String toggleAB();

private:
    void timerCallback() override;

    struct ABSnapshot
    {
        std::array<float, 8> values {};
        std::array<bool, 7> moduleEnabled {};
    };

    ABSnapshot captureSnapshot() const;
    void applySnapshot (const ABSnapshot&);

    ABSnapshot abSlotA, abSlotB;
    bool abActiveIsA = true;
    // Only ever navigates to our own embedded resource root - the frontend
    // cannot be redirected to an external site.
    struct SinglePageBrowser final : juce::WebBrowserComponent
    {
        using WebBrowserComponent::WebBrowserComponent;
        bool pageAboutToLoad (const juce::String& newURL) override;
    };

    UNI76AudioProcessor& processor;

    juce::WebSliderRelay preampRelay     { "preamp" };
    juce::WebSliderRelay eqRelay         { "eq" };
    juce::WebSliderRelay saturationRelay { "saturation" };
    juce::WebSliderRelay pitchRelay      { "pitch" };
    juce::WebSliderRelay panoramaRelay   { "panorama" };
    juce::WebSliderRelay reverbRelay     { "reverb" };
    juce::WebSliderRelay imagerRelay     { "imager" };
    juce::WebSliderRelay imageTiltRelay  { "imageTilt" };

    juce::WebControlParameterIndexReceiver controlParameterIndexReceiver;

    SinglePageBrowser webView;

    juce::WebSliderParameterAttachment preampAttachment;
    juce::WebSliderParameterAttachment eqAttachment;
    juce::WebSliderParameterAttachment saturationAttachment;
    juce::WebSliderParameterAttachment pitchAttachment;
    juce::WebSliderParameterAttachment panoramaAttachment;
    juce::WebSliderParameterAttachment reverbAttachment;
    juce::WebSliderParameterAttachment imagerAttachment;
    juce::WebSliderParameterAttachment imageTiltAttachment;

    // Message-thread-only envelope state for the meter telemetry timer -
    // fast attack, slower release, applied here (not in JS, not on the
    // audio thread) so the smoothing survives even when the browser is
    // briefly not rendering.
    float inputMeterEnvelope = 0.0f;
    float outputMeterEnvelope = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UNI76AudioProcessorEditor)
};
