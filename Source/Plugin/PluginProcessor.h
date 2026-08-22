#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/*
    UNI 76 - root AudioProcessor.

    Stage-1 foundation rules for this file (see CLAUDE.md for the full
    policy):
      - processBlock() is a strictly transparent passthrough: input == output,
        no gain, no latency, no DSP, no allocations, no locks, no file I/O,
        and no calls into the WebView/GUI layer.
      - The 7 public parameters exist and are DAW-automation compatible via
        APVTS, but none of them influence the audio signal yet.
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

private:
    juce::AudioProcessorValueTreeState apvts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UNI76AudioProcessor)
};
