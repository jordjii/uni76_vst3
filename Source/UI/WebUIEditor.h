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

    // Startup profiling (see docs/FULL_DSP_AUDIT.md's GUI-startup
    // measurements) - called from the free-function native-function lambda
    // in WebUIEditor.cpp, so public for the same reason toggleAB() is.
    void reportStartupTiming (double jsT0, double jsDomContentLoaded, double jsAppReady);

    // ---- Active-preset identity (item 4 of the UX polish pass) --------
    // Tracked separately from the raw parameter state so the PRESET menu
    // can keep the real current preset highlighted, and so the header/
    // footer can show "Name" vs "Name *" (dirty - live values no longer
    // match what was loaded) without ever doing an expensive preset-table
    // search on every parameter callback: dirty is just 10 float + 7 bool
    // compares against the snapshot captured at the moment the preset was
    // applied, done at most once per 30Hz timer tick (see timerCallback()),
    // not per callback.
    enum class PresetKind { none, factory, user };

    struct ActivePresetInfo
    {
        PresetKind kind = PresetKind::none;
        juce::String name;
        bool dirty = false;
    };

    void setActivePreset (PresetKind kind, const juce::String& name);
    void clearActivePreset();
    ActivePresetInfo getActivePresetInfo() const;

private:
    void timerCallback() override;

public:
    struct ABSnapshot
    {
        std::array<float, 10> values {};
        std::array<bool, 7> moduleEnabled {};
    };

private:
    ABSnapshot captureSnapshot() const;
    void applySnapshot (const ABSnapshot&);
    bool snapshotsEqual (const ABSnapshot&, const ABSnapshot&) const;

    ABSnapshot abSlotA, abSlotB;
    bool abActiveIsA = true;

    PresetKind activePresetKind = PresetKind::none;
    juce::String activePresetName;
    ABSnapshot activePresetSnapshot;

    // Only ever navigates to our own embedded resource root - the frontend
    // cannot be redirected to an external site.
    struct SinglePageBrowser final : juce::WebBrowserComponent
    {
        using WebBrowserComponent::WebBrowserComponent;
        bool pageAboutToLoad (const juce::String& newURL) override;
    };

    double constructionStartMs;
    UNI76AudioProcessor& processor;

    juce::WebSliderRelay preampRelay     { "preamp" };
    juce::WebSliderRelay eqRelay         { "eq" };
    juce::WebSliderRelay saturationRelay { "saturation" };
    juce::WebSliderRelay pitchRelay      { "pitch" };
    juce::WebSliderRelay panoramaRelay   { "panorama" };
    juce::WebSliderRelay reverbRelay     { "reverb" };
    juce::WebSliderRelay imagerRelay     { "imager" };
    juce::WebSliderRelay imageTiltRelay  { "imageTilt" };
    juce::WebSliderRelay panRateRelay    { "panRate" };
    juce::WebSliderRelay verbDriveRelay  { "verbDrive" };

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
    juce::WebSliderParameterAttachment panRateAttachment;
    juce::WebSliderParameterAttachment verbDriveAttachment;

    // Message-thread-only envelope state for the meter telemetry timer -
    // fast attack, slower release, applied here (not in JS, not on the
    // audio thread) so the smoothing survives even when the browser is
    // briefly not rendering.
    float inputMeterEnvelope = 0.0f;
    float outputMeterEnvelope = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UNI76AudioProcessorEditor)
};
