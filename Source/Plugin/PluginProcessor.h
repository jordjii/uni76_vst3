#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Core/ChainOrder.h"
#include "Core/LevelMeter.h"
#include "Core/LicenseState.h"
#include "Core/ModuleEnableState.h"
#include "DSP/PreampProcessor.h"
#include "DSP/EqProcessor.h"
#include "DSP/SatProcessor.h"
#include "DSP/PitchProcessor.h"
#include "DSP/PanoramaProcessor.h"
#include "DSP/VerbProcessor.h"
#include "DSP/ImagerProcessor.h"
#include "DSP/DelayProcessor.h"

/*
    UNI 76 - root AudioProcessor.

    Foundation rules for this file (see CLAUDE.md for the full policy):
      - processBlock() must stay realtime-safe: no allocations, no locks,
        no file I/O, no calls into the WebView/GUI layer. The one exception
        is the INPUT/OUTPUT level meters: processBlock() pushes each
        block's peak into a lock-free uni76::LevelMeter (atomic, no
        allocation, no locking - see Core/LevelMeter.h). The editor's UI
        timer, running on the message thread, is what actually reads those
        meters and talks to the WebView - the audio thread itself never
        touches the UI.
      - PREAMP, EQ, SAT, PITCH, PAN, VERB, IMAGE and DELAY all have real
        DSP (see Source/DSP/PreampProcessor.h + docs/DSP_PREAMP.md,
        Source/DSP/EqProcessor.h + docs/DSP_EQ.md,
        Source/DSP/SatProcessor.h + docs/DSP_SAT.md,
        Source/DSP/PitchProcessor.h + docs/DSP_PITCH.md,
        Source/DSP/PanoramaProcessor.h + docs/DSP_PAN.md,
        Source/DSP/VerbProcessor.h + docs/DSP_VERB.md,
        Source/DSP/ImagerProcessor.h + docs/DSP_IMAGE.md, and
        Source/DSP/DelayProcessor.h + docs/DSP_DELAY.md). IMAGE is a
        deliberate exception to the "one knob per module" rule - it has
        two independent public parameters, `imager` (width/imaging
        amount) and `imageTilt` (static L/R balance) - see
        docs/DSP_IMAGE.md. DELAY (added 2026-09-14) is the 8th module -
        see docs/DSP_DELAY.md.
*/

class UNI76AudioProcessor final : public juce::AudioProcessor
{
public:
    UNI76AudioProcessor();
    ~UNI76AudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;

    /** VERB is the only module with a genuine decaying tail (the FDN
        plate tank) - every other module is a pure gain/filter morph or
        fixed-latency processing chain, not a source of output that
        continues after input stops. Computed dynamically from VERB's
        current wet amount and enabled state (see VerbCurves.h's
        verbDecaySeconds) rather than a fixed constant, so a host doesn't
        truncate a real, audible tail at DEEP/100% - see docs/DSP_VERB.md
        and docs/FULL_DSP_AUDIT.md's "VERB tail" section. */
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getValueTreeState() noexcept { return apvts; }

    /** Peak measured at the top of processBlock(), before any future processing chain. */
    uni76::LevelMeter& getInputLevelMeter() noexcept { return inputLevelMeter; }
    /** Peak measured at the bottom of processBlock(), after any future processing chain. */
    uni76::LevelMeter& getOutputLevelMeter() noexcept { return outputLevelMeter; }

    /** Persistent (state-saved) but non-automatable per-module on/off flags - see Core/ModuleEnableState.h. */
    uni76::ModuleEnableState& getModuleEnableState() noexcept { return moduleEnableState; }

    /** Persistent (state-saved) but non-automatable module processing
        order - drag-and-drop pedalboard reordering, see Core/ChainOrder.h.
        processBlock() dispatches through this every block, so reordering
        genuinely changes the signal path, not just the on-screen layout. */
    uni76::ChainOrder& getChainOrder() noexcept { return chainOrder; }

    /** Recomputes and reports the plugin's total latency from every
        module's own getLatencySamples() plus the *current* module-enable
        state - must be called (from the message thread, never
        processBlock()) any time `moduleEnableState`'s pitch flag could
        have changed: after prepareToPlay(), after setStateInformation(),
        and after every moduleEnableState.setEnabled() call the UI/preset/
        A-B code makes (see WebUIEditor.cpp's call sites). PITCH is the
        one module whose reported contribution depends on its own enabled
        flag - see docs/DSP_PITCH.md's "Zero-latency bypass" section for
        why (every other latency-owning module's own latency is
        architecturally always-on, so summing it unconditionally is still
        correct for them). Public because WebUIEditor.cpp must call it. */
    void updateReportedLatency() noexcept;

    /** Persistent (state-saved) identity of the last-applied preset - kind
        (0=none, 1=factory, 2=user; mirrors WebUIEditor::PresetKind, kept
        as a plain int here so the processor doesn't need to depend on an
        editor-side enum) and display name. Tracked here, not just on the
        editor, specifically so the name survives editor close/reopen -
        see PluginIdentity.h's activePresetKindProperty/activePresetNameProperty
        and getStateInformation()/setStateInformation(). Does not track the
        preset's own snapshot of parameter values (unlike the editor's
        richer ActivePresetInfo, which also computes a "dirty" flag) - on
        editor reopen the current live values are treated as the new clean
        baseline for that comparison, a deliberate small simplification
        (the dirty asterisk may reset across a close/reopen even if it was
        set beforehand; the preset *name* itself, which is what was
        actually reported missing, is fully preserved). */
    void setActivePresetInfo (int kind, const juce::String& name) noexcept { activePresetKind = kind; activePresetName = name; }
    int getActivePresetKind() const noexcept { return activePresetKind; }
    const juce::String& getActivePresetName() const noexcept { return activePresetName; }

    /** Offline, machine-bound license check - see Core/LicenseState.h.
        Realtime-safe (atomic read only); processBlock() mutes entirely
        when this is false. */
    bool isLicensed() const noexcept { return licenseState.isLicensed(); }

    /** Runs the real, file-backed license check (see Core/LicenseState.h).
        Message-thread only - called exactly once, by createPluginFilter()
        immediately after construction (see that function's own comment
        for why this is deliberately not in the constructor itself). */
    void refreshLicenseState() { licenseState.refresh(); }

private:
    juce::AudioProcessorValueTreeState apvts;

    // Telemetry only - never part of getStateInformation()/setStateInformation(),
    // never an APVTS parameter, never read back by the audio thread itself.
    uni76::LevelMeter inputLevelMeter;
    uni76::LevelMeter outputLevelMeter;

    // Persisted as plain properties on the saved state ValueTree (see
    // getStateInformation/setStateInformation) but deliberately not part
    // of the APVTS parameter tree - see Core/ModuleEnableState.h.
    uni76::ModuleEnableState moduleEnableState;

    // Same persistence pattern as moduleEnableState above - see
    // Core/ChainOrder.h.
    uni76::ChainOrder chainOrder;

    // Same persistence pattern again - see setActivePresetInfo()'s doc
    // comment above.
    int activePresetKind = 0;
    juce::String activePresetName;

    // NOT part of the saved state ValueTree at all, unlike everything else
    // above - a license is machine-wide, not project-wide, so it's always
    // re-read fresh from disk (see refresh(), called once in the
    // constructor) rather than round-tripped through
    // getStateInformation()/setStateInformation(). See Core/LicenseState.h.
    uni76::LicenseState licenseState;

    uni76::dsp::PreampProcessor preampProcessor;
    uni76::dsp::EqProcessor eqProcessor;
    uni76::dsp::SatProcessor satProcessor;
    uni76::dsp::PitchProcessor pitchProcessor;
    uni76::dsp::PanoramaProcessor panoramaProcessor;
    uni76::dsp::VerbProcessor verbProcessor;
    uni76::dsp::ImagerProcessor imagerProcessor;
    uni76::dsp::DelayProcessor delayProcessor;

    // Cached raw parameter pointers (juce::AudioProcessorValueTreeState's
    // documented realtime-safe way to read a parameter's current value
    // from processBlock - no lock, no allocation).
    std::atomic<float>* preampParameter = nullptr;
    std::atomic<float>* eqParameter = nullptr;
    std::atomic<float>* saturationParameter = nullptr;
    std::atomic<float>* pitchParameter = nullptr;
    std::atomic<float>* panoramaParameter = nullptr;
    std::atomic<float>* reverbParameter = nullptr;
    std::atomic<float>* imagerParameter = nullptr;
    std::atomic<float>* imageTiltParameter = nullptr;
    std::atomic<float>* panRateParameter = nullptr;
    std::atomic<float>* verbDriveParameter = nullptr;
    std::atomic<float>* delayParameter = nullptr;
    std::atomic<float>* delayFeedbackParameter = nullptr;
    std::atomic<float>* delayDivisionParameter = nullptr;
    std::atomic<float>* delayStereoParameter = nullptr;
    std::atomic<float>* delayPingPongParameter = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UNI76AudioProcessor)
};
