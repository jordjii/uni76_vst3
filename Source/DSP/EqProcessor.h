#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

#include "Biquad.h"

/*
    UNI 76's EQ / TONE module - the second real DSP in the plugin (after
    PREAMP). SAT/PITCH/PAN/VERB/IMAGE stay passthrough - see CLAUDE.md and
    docs/DSP_EQ.md.

    A fixed five-stage minimum-phase filter network (HP -> low shelf ->
    bell/presence -> high shelf -> LP), always present - the EQ/TONE
    parameter (0..1) only ever morphs each stage's own frequency/gain/Q
    continuously (see Source/DSP/EqCurves.h), never swaps topology. No
    oversampling, no nonlinearity - PREAMP (and the future SAT module) own
    character/harmonics; EQ only shapes frequency response.

    Realtime-safety contract matches PreampProcessor: prepare() is the
    only place that allocates; process() never allocates, locks, or
    touches the filesystem/WebView.
*/

namespace uni76::dsp
{
    class EqProcessor
    {
    public:
        EqProcessor() = default;

        void prepare (double sampleRate, int maximumBlockSize, int numChannelsToUse);
        void reset() noexcept;

        void process (juce::AudioBuffer<float>& buffer, float eqNormalised01, bool enabled) noexcept;

        /** Always 0 - a fixed IIR filter network with no oversampling adds
            no algorithmic latency. */
        int getLatencySamples() const noexcept { return 0; }

    private:
        void updateCoefficients (float eqForCoefficients) noexcept;

        static constexpr int maxChannels = 2;

        double sampleRate = 44100.0;
        int numChannels = 2;
        float outputTrimGainLinear = 1.0f;

        std::array<Biquad, maxChannels> hpFilters;
        std::array<Biquad, maxChannels> lowShelfFilters;
        std::array<Biquad, maxChannels> bellFilters;
        std::array<Biquad, maxChannels> highShelfFilters;
        std::array<Biquad, maxChannels> lpFilters;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> eqSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassSmoother;

        std::vector<float> bypassRampScratch;
        juce::AudioBuffer<float> dryScratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqProcessor)
    };
}
