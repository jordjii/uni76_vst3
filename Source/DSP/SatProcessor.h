#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <memory>
#include <vector>

#include "Biquad.h"

/*
    UNI 76's SAT / ANALOG DRIVE module - the third real DSP in the plugin
    (after PREAMP and EQ). PITCH/PAN/VERB/IMAGE stay passthrough - see
    CLAUDE.md and docs/DSP_SAT.md.

    Deliberately NOT a second PREAMP. PREAMP is a static per-sample
    transformer/front-end waveshaper (H2/H3 coloration + drive-dependent
    Low/High Cut). SAT is an energy-dependent saturation/compression
    stage - it tracks short-term signal level and uses it to drive a soft
    dynamic gain stage ahead of its own (differently-tuned, more
    aggressive) bounded waveshaper, surrounded by a frequency tilt that
    protects bass and softens highs as HEAT increases:

        Input
          -> DC protection (fixed ~5 Hz one-pole HPF, base rate)
          -> [oversampled region]
               -> frequency tilt pre-emphasis (low-shelf cut + high-shelf
                  boost, both drive-dependent, exact inverse applied after)
               -> envelope-driven dynamic gain (soft "glue" compression -
                  see EnvelopeFollower in Biquad.h)
               -> nonlinear analog stage (asymmetric-gain tanh waveshaper,
                  same bounded-per-half-gain structure PREAMP's
                  calibration pass settled on, own constants)
               -> frequency tilt de-emphasis (exact algebraic inverse of
                  the pre-emphasis pair)
          -> output compensation (drive-dependent trim)
          -> enable/disable crossfade against a latency-aligned dry copy
          -> Output

    Realtime-safety contract matches PreampProcessor/EqProcessor:
    prepare() is the only place that allocates; process() never
    allocates, locks, or touches the filesystem/WebView.
*/

namespace uni76::dsp
{
    class SatProcessor
    {
    public:
        SatProcessor() = default;

        void prepare (double sampleRate, int maximumBlockSize, int numChannelsToUse);
        void reset() noexcept;

        /** heatNormalised01 and enabled are read once per call - both are
            smoothed internally, so passing a raw (possibly jumpy)
            automation value each block is safe and expected. */
        void process (juce::AudioBuffer<float>& buffer, float heatNormalised01, bool enabled) noexcept;

        /** Constant for the lifetime of a prepare() call - depends only on
            sample rate (oversampling factor), never on HEAT or enabled, so
            the host's plugin-delay-compensation stays valid throughout. */
        int getLatencySamples() const noexcept { return latencySamples; }

    private:
        void updateBlockCoefficients (float heatForCoefficients) noexcept;
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
        /** A second DC blocker *after* the nonlinearity. The one above
            only protects the waveshaper from DC arriving at the input;
            it cannot remove the DC the asymmetric waveshaper itself
            generates, which is a real offset that eats headroom and was
            found by the HEAT=100% DC test once the drive curve was
            front-loaded (see SatCurves.h). PREAMP doesn't need an
            equivalent only because its own post-waveshaper Low Cut
            (a 20-70Hz highpass) already removes DC as a side effect -
            SAT has no highpass after its shaper, just the de-emphasis
            shelves, so it needs this explicitly. */
        std::array<DcBlocker, maxChannels> outputDcBlockers;

        std::array<Biquad, maxChannels> lowShelfPre, highShelfPre;
        std::array<Biquad, maxChannels> highShelfDe, lowShelfDe;

        std::array<EnvelopeFollower, maxChannels> envelopeFollowers;

        std::array<IntegerDelayLine, maxChannels> dryDelays;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> heatSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassSmoother;

        std::vector<float> heatRampScratch;
        std::vector<float> bypassRampScratch;

        juce::AudioBuffer<float> dryScratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SatProcessor)
    };
}
