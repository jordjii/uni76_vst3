#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

#include "Biquad.h"

/*
    UNI 76's EQ / PHONE TONE module - the second real DSP in the plugin
    (after PREAMP). SAT/PITCH/PAN/VERB/IMAGE stay passthrough - see
    CLAUDE.md and docs/DSP_EQ.md.

    An always-on, steep two-cut "telephone band" filter (HP + LP, each
    48dB/oct via 4 cascaded 2nd-order stages) - the EQ/PHONE TONE
    parameter (0..1, displayed -50%..+50%) sweeps both cuts' corner
    frequencies together, always keeping the same ratio between them (see
    Source/DSP/EqCurves.h), never swaps topology. No oversampling, no
    nonlinearity - PREAMP/SAT own character/harmonics; EQ only shapes
    frequency response.

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
        static constexpr int stagesPerCut = 4; // 4 x 2nd-order (12dB/oct) = 48dB/oct

        double sampleRate = 44100.0;
        int numChannels = 2;

        std::array<std::array<Biquad, stagesPerCut>, maxChannels> hpStages;
        std::array<std::array<Biquad, stagesPerCut>, maxChannels> lpStages;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> eqSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassSmoother;

        std::vector<float> bypassRampScratch;
        juce::AudioBuffer<float> dryScratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqProcessor)
    };
}
