#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Core/LevelMeter.h"
#include "Core/ModuleEnableState.h"
#include "DSP/PreampProcessor.h"
#include "DSP/EqProcessor.h"
#include "DSP/SatProcessor.h"
#include "DSP/PitchProcessor.h"
#include "DSP/PanoramaProcessor.h"
#include "DSP/VerbProcessor.h"

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
      - PREAMP, EQ, SAT, PITCH, PAN and VERB are the only modules with
        real DSP so far (see Source/DSP/PreampProcessor.h + docs/DSP_PREAMP.md,
        Source/DSP/EqProcessor.h + docs/DSP_EQ.md,
        Source/DSP/SatProcessor.h + docs/DSP_SAT.md,
        Source/DSP/PitchProcessor.h + docs/DSP_PITCH.md,
        Source/DSP/PanoramaProcessor.h + docs/DSP_PAN.md, and
        Source/DSP/VerbProcessor.h + docs/DSP_VERB.md). Imager remains a
        strict passthrough - its parameter exists and is DAW-automation
        compatible via APVTS, but does not yet influence the audio
        signal.
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

    uni76::dsp::PreampProcessor preampProcessor;
    uni76::dsp::EqProcessor eqProcessor;
    uni76::dsp::SatProcessor satProcessor;
    uni76::dsp::PitchProcessor pitchProcessor;
    uni76::dsp::PanoramaProcessor panoramaProcessor;
    uni76::dsp::VerbProcessor verbProcessor;

    // Cached raw parameter pointers (juce::AudioProcessorValueTreeState's
    // documented realtime-safe way to read a parameter's current value
    // from processBlock - no lock, no allocation).
    std::atomic<float>* preampParameter = nullptr;
    std::atomic<float>* eqParameter = nullptr;
    std::atomic<float>* saturationParameter = nullptr;
    std::atomic<float>* pitchParameter = nullptr;
    std::atomic<float>* panoramaParameter = nullptr;
    std::atomic<float>* reverbParameter = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UNI76AudioProcessor)
};
