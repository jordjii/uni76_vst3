#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Core/LevelMeter.h"
#include "Core/ModuleEnableState.h"

/*
    UNI 76 - root AudioProcessor.

    Stage-1 foundation rules for this file (see CLAUDE.md for the full
    policy):
      - processBlock() is a strictly transparent passthrough: input == output,
        no gain, no latency, no DSP, no allocations, no locks, no file I/O,
        and no calls into the WebView/GUI layer.
      - The 7 public parameters exist and are DAW-automation compatible via
        APVTS, but none of them influence the audio signal yet.
      - The one exception to "no calls into the WebView/GUI layer" is the
        INPUT/OUTPUT level meters: processBlock() pushes the block's peak
        into a lock-free uni76::LevelMeter (atomic, no allocation, no
        locking - see Core/LevelMeter.h). The editor's UI timer, running on
        the message thread, is what actually reads those meters and talks
        to the WebView - the audio thread itself never touches the UI.
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UNI76AudioProcessor)
};
