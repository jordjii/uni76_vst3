#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <memory>
#include <vector>

#include "Biquad.h"

/*
    UNI 76's PREAMP / TRANSFORMER module - the first real DSP in the
    plugin. Everything else (EQ/SAT/PITCH/PAN/VERB/IMAGE) stays passthrough
    - see CLAUDE.md and docs/DSP_PREAMP.md.

    Signal chain (see docs/DSP_PREAMP.md for the full rationale and
    measured curves):

        Input
          -> DC / infrasonic protection (fixed ~5 Hz one-pole HPF)
          -> [oversampled region]
               -> transformer coloration (one-pole "core" rounding filter
                  + fixed-frequency low-shelf density boost, both
                  drive-dependent)
               -> nonlinear analog stage (asymmetric tanh waveshaper,
                  normalised so DRIVE=0 is structurally near-identity)
          -> soft Low Cut (drive-dependent, ~20-70 Hz)
          -> soft High Cut (drive-dependent, ~20k-11k Hz)
          -> output compensation (small drive-dependent trim)
          -> enable/disable crossfade against a latency-aligned dry copy
          -> Output

    Realtime-safety contract: prepare() is the only place that allocates
    (scratch buffers, the Oversampling object, filter/delay-line storage
    sized once). process() never allocates, locks, or touches the
    filesystem/WebView - see PluginProcessor::processBlock.
*/

namespace uni76::dsp
{
    class PreampProcessor
    {
    public:
        PreampProcessor() = default;

        void prepare (double sampleRate, int maximumBlockSize, int numChannelsToUse);
        void reset() noexcept;

        /** driveNormalised01 and enabled are read once per call - both are
            smoothed internally, so passing a raw (possibly jumpy) automation
            value each block is safe and expected. */
        void process (juce::AudioBuffer<float>& buffer, float driveNormalised01, bool enabled) noexcept;

        /** Constant for the lifetime of a prepare() call - depends only on
            sample rate (oversampling factor), never on DRIVE or enabled, so
            the host's plugin-delay-compensation stays valid throughout. */
        int getLatencySamples() const noexcept { return latencySamples; }

    private:
        void updateBlockCoefficients (float driveForCoefficients) noexcept;
        static int chooseOversamplingStages (double sampleRate) noexcept;

        static constexpr int maxChannels = 2;

        double sampleRate = 44100.0;
        double oversampledRate = 44100.0;
        int oversamplingStages = 0;
        int latencySamples = 0;
        int numChannels = 2;
        int maxOversampledBlockSize = 0;

        std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;

        std::array<DcBlocker, maxChannels> dcBlockers;

        std::array<OnePoleLowPass, maxChannels> roundingFilters;
        std::array<Biquad, maxChannels> colorShelf;

        std::array<Biquad, maxChannels> lowCutFilters;
        std::array<Biquad, maxChannels> highCutFilters;

        std::array<IntegerDelayLine, maxChannels> dryDelays;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassSmoother;

        std::vector<float> driveRampScratch;
        std::vector<float> bypassRampScratch;

        juce::AudioBuffer<float> dryScratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreampProcessor)
    };
}
