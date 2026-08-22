#include "PreampProcessor.h"
#include "PreampCurves.h"

#include <cmath>

namespace uni76::dsp
{
    int PreampProcessor::chooseOversamplingStages (double sr) noexcept
    {
        // 44.1/48kHz  -> 2 stages (4x)
        // 88.2/96kHz  -> 1 stage  (2x, reduced factor - already has headroom)
        // 176.4/192kHz+ -> 0 stages (no added oversampling - would be pointless)
        if (sr <= 48000.0 + 1.0)
            return 2;
        if (sr <= 96000.0 + 1.0)
            return 1;
        return 0;
    }

    void PreampProcessor::prepare (double sampleRateIn, int maximumBlockSize, int numChannelsToUse)
    {
        sampleRate = sampleRateIn;
        numChannels = juce::jlimit (1, maxChannels, numChannelsToUse);

        oversamplingStages = chooseOversamplingStages (sampleRate);
        const auto factor = 1 << oversamplingStages;
        oversampledRate = sampleRate * (double) factor;

        if (oversamplingStages > 0)
        {
            oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
                (size_t) numChannels,
                (size_t) oversamplingStages,
                juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                true,   // isMaximumQuality - solid alias rejection given we deliberately generate harmonics
                true);  // useIntegerLatency - exact latency for correct host PDC

            oversampler->initProcessing ((size_t) maximumBlockSize);
            latencySamples = (int) oversampler->getLatencyInSamples();
        }
        else
        {
            oversampler.reset();
            latencySamples = 0;
        }

        maxOversampledBlockSize = maximumBlockSize * factor;

        driveRampScratch.assign ((size_t) maxOversampledBlockSize, 0.0f);
        bypassRampScratch.assign ((size_t) maximumBlockSize, 0.0f);

        dryScratch.setSize (numChannels, maximumBlockSize, false, false, true);

        driveSmoother.reset (oversampledRate, preampDriveSmoothingSeconds);
        bypassSmoother.reset (sampleRate, preampBypassSmoothingSeconds);
        driveSmoother.setCurrentAndTargetValue (0.0f);
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            dcBlockers[(size_t) ch].setCutoffHz (sampleRate, preampDcBlockerHz);
            dryDelays[(size_t) ch].prepare (latencySamples);
        }

        reset();
        updateBlockCoefficients (0.0f);
    }

    void PreampProcessor::reset() noexcept
    {
        for (int ch = 0; ch < maxChannels; ++ch)
        {
            dcBlockers[(size_t) ch].reset();
            roundingFilters[(size_t) ch].reset();
            colorShelf[(size_t) ch].reset();
            lowCutFilters[(size_t) ch].reset();
            highCutFilters[(size_t) ch].reset();
            dryDelays[(size_t) ch].reset();
        }

        if (oversampler != nullptr)
            oversampler->reset();
    }

    void PreampProcessor::updateBlockCoefficients (float driveForCoefficients) noexcept
    {
        const auto t = clamp01 (driveForCoefficients);

        const auto roundingHz  = preampRoundingCutoffHz (t);
        const auto shelfGainDb = preampColorShelfGainDb (t);
        const auto lowCutHz    = preampLowCutHz (t);
        const auto highCutHz   = preampHighCutHz (t);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            roundingFilters[(size_t) ch].setCutoffHz (oversampledRate, roundingHz);
            makeLowShelf (colorShelf[(size_t) ch], oversampledRate, preampColorShelfFreqHz, shelfGainDb);
            makeHighPassButterworth (lowCutFilters[(size_t) ch], sampleRate, lowCutHz);
            makeLowPassButterworth  (highCutFilters[(size_t) ch], sampleRate, highCutHz);
        }
    }

    void PreampProcessor::process (juce::AudioBuffer<float>& buffer, float driveNormalised01, bool enabled) noexcept
    {
        const auto numSamples = buffer.getNumSamples();
        const auto channels   = juce::jmin (numChannels, buffer.getNumChannels());

        if (numSamples <= 0 || channels <= 0)
            return;

        // ---- numeric safety at the input boundary --------------------
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                if (! std::isfinite (data[i]))
                    data[i] = 0.0f;
        }

        // ---- latency-aligned dry copy for the enable/disable crossfade -
        // Always pushed through the same fixed delay the wet path incurs
        // from oversampling, so bypassing PREAMP never shifts the plugin's
        // effective output timing relative to what getLatencySamples()
        // told the host - see Source/DSP/Biquad.h's IntegerDelayLine.
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* dry = dryScratch.getWritePointer (ch);
            const auto* in = buffer.getReadPointer (ch);

            for (int i = 0; i < numSamples; ++i)
                dry[i] = dryDelays[(size_t) ch].processSample (in[i]);
        }

        // ---- smoothing targets -----------------------------------------
        // Filter shapes (Low/High Cut, colour, rounding) are recomputed
        // once per block from the *previous* block's settled drive value -
        // a one-block lag that's inaudible and standard practice, avoiding
        // per-sample trig cost. Gain-critical parts (drive amount, output
        // trim) use a true sample-accurate ramp below instead, which is
        // where zipper noise would actually be audible.
        const auto driveForCoefficients = driveSmoother.getCurrentValue();
        driveSmoother.setTargetValue (clamp01 (driveNormalised01));
        bypassSmoother.setTargetValue (enabled ? 1.0f : 0.0f);

        updateBlockCoefficients (driveForCoefficients);

        // ---- DC / infrasonic protection (base rate, fixed) --------------
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                data[i] = dcBlockers[(size_t) ch].processSample (data[i]);
        }

        // ---- upsample -----------------------------------------------
        juce::dsp::AudioBlock<float> block (buffer);
        auto processingBlock = oversampler != nullptr ? oversampler->processSamplesUp (block) : block;
        const auto oversampledNumSamples = (int) processingBlock.getNumSamples();

        for (int i = 0; i < oversampledNumSamples; ++i)
            driveRampScratch[(size_t) i] = driveSmoother.getNextValue();

        // ---- transformer coloration + nonlinear analog stage (oversampled) -
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = processingBlock.getChannelPointer ((size_t) ch);
            auto& rounding = roundingFilters[(size_t) ch];
            auto& shelf    = colorShelf[(size_t) ch];

            for (int i = 0; i < oversampledNumSamples; ++i)
            {
                const auto t = driveRampScratch[(size_t) i];

                auto x = data[i];
                x = rounding.processSample (x);
                x = shelf.processSample (x);

                const auto driveGain = preampDriveGainLinear (t);
                const auto xd = x * driveGain;
                // Quadratic term is asymmetric (doesn't flip sign with x),
                // producing the even-harmonic (H2) content real tanh() odd
                // symmetry can't - scaled small enough to stay a colour,
                // never the dominant term.
                const auto xa = xd + preampAsymmetryAmount (t) * xd * xd;

                // Normalising by tanh(driveGain) rather than driveGain
                // itself keeps DRIVE=0 structurally near-identity (small
                // driveGain => tanh(g*x)/tanh(g) ~ x) while DRIVE=100 gets
                // a firm, soft (never hard-clipped) ceiling near unity.
                const auto norm = std::tanh (driveGain);
                const auto y = norm > 1.0e-6f ? std::tanh (xa) / norm : xa;

                data[i] = y;
            }
        }

        // ---- downsample -----------------------------------------------
        if (oversampler != nullptr)
            oversampler->processSamplesDown (block);

        // ---- soft Low Cut / High Cut (drive-dependent, base rate) ------
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            auto& lowCut  = lowCutFilters[(size_t) ch];
            auto& highCut = highCutFilters[(size_t) ch];

            for (int i = 0; i < numSamples; ++i)
            {
                auto y = data[i];
                y = lowCut.processSample (y);
                y = highCut.processSample (y);
                data[i] = y;
            }
        }

        // ---- output compensation (small drive-dependent trim) ----------
        const auto factor = oversamplingStages > 0 ? (1 << oversamplingStages) : 1;
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                const auto rampIndex = juce::jlimit (0, oversampledNumSamples - 1, i * factor);
                const auto t = driveRampScratch[(size_t) rampIndex];
                data[i] *= juce::Decibels::decibelsToGain (preampOutputCompensationDb (t));
            }
        }

        // ---- enable/disable crossfade (base rate, sample-accurate) -----
        for (int i = 0; i < numSamples; ++i)
            bypassRampScratch[(size_t) i] = bypassSmoother.getNextValue();

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* wet = buffer.getWritePointer (ch);
            const auto* dry = dryScratch.getReadPointer (ch);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto mix = bypassRampScratch[(size_t) i];
                wet[i] = dry[i] * (1.0f - mix) + wet[i] * mix;
            }
        }

        // ---- final numeric safety net ------------------------------------
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                if (! std::isfinite (data[i]))
                    data[i] = 0.0f;
        }
    }
}
