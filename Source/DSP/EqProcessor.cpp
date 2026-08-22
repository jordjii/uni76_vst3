#include "EqProcessor.h"
#include "EqCurves.h"

#include <cmath>

namespace uni76::dsp
{
    void EqProcessor::prepare (double sampleRateIn, int maximumBlockSize, int numChannelsToUse)
    {
        sampleRate = sampleRateIn;
        numChannels = juce::jlimit (1, maxChannels, numChannelsToUse);

        bypassRampScratch.assign ((size_t) maximumBlockSize, 0.0f);
        dryScratch.setSize (numChannels, maximumBlockSize, false, false, true);

        eqSmoother.reset (sampleRate, eqParameterSmoothingSeconds);
        bypassSmoother.reset (sampleRate, eqBypassSmoothingSeconds);
        eqSmoother.setCurrentAndTargetValue (0.5f); // matches the "eq" parameter's own 50% (PHONE) default
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        reset();
        updateCoefficients (0.5f);
    }

    void EqProcessor::reset() noexcept
    {
        for (int ch = 0; ch < maxChannels; ++ch)
        {
            hpFilters[(size_t) ch].reset();
            lowShelfFilters[(size_t) ch].reset();
            bellFilters[(size_t) ch].reset();
            highShelfFilters[(size_t) ch].reset();
            lpFilters[(size_t) ch].reset();
        }
    }

    void EqProcessor::updateCoefficients (float eqForCoefficients) noexcept
    {
        const auto params = eqParamsAt (eqForCoefficients);
        outputTrimGainLinear = juce::Decibels::decibelsToGain (params.outputTrimDb);

        // Clamp every stage's frequency to a safe fraction of Nyquist so
        // the network stays stable and musically meaningful at every
        // supported sample rate - AIR's nominal ~20kHz LP target, for
        // instance, would sit uncomfortably close to Nyquist at 44.1kHz
        // without this.
        const auto nyquist = (float) (sampleRate * 0.5);
        const auto safeMax = nyquist * 0.9f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            makeHighPassButterworth (hpFilters[(size_t) ch],       sampleRate, juce::jlimit (5.0f, safeMax, params.hpHz));
            makeLowShelf           (lowShelfFilters[(size_t) ch],  sampleRate, juce::jlimit (20.0f, safeMax, params.lowShelfHz), params.lowShelfDb);
            makePeakingEq          (bellFilters[(size_t) ch],      sampleRate, juce::jlimit (20.0f, safeMax, params.bellHz), params.bellDb, params.bellQ);
            makeHighShelf          (highShelfFilters[(size_t) ch], sampleRate, juce::jlimit (20.0f, safeMax, params.highShelfHz), params.highShelfDb);
            makeLowPassButterworth (lpFilters[(size_t) ch],        sampleRate, juce::jlimit (20.0f, safeMax, params.lpHz));
        }
    }

    void EqProcessor::process (juce::AudioBuffer<float>& buffer, float eqNormalised01, bool enabled) noexcept
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

        // ---- dry copy for the enable/disable crossfade -----------------
        // No delay-alignment needed here (unlike PREAMP): EQ adds zero
        // algorithmic latency, so the dry and wet paths are already
        // time-aligned sample-for-sample.
        for (int ch = 0; ch < channels; ++ch)
            dryScratch.copyFrom (ch, 0, buffer, ch, 0, numSamples);

        // ---- smoothing targets -----------------------------------------
        // Filter shapes are recomputed once per block from the *previous*
        // block's settled value - a one-block lag that's inaudible and
        // standard practice (same pattern as PreampProcessor).
        const auto eqForCoefficients = eqSmoother.getCurrentValue();
        eqSmoother.setTargetValue (juce::jlimit (0.0f, 1.0f, eqNormalised01));
        bypassSmoother.setTargetValue (enabled ? 1.0f : 0.0f);

        updateCoefficients (eqForCoefficients);
        eqSmoother.skip (numSamples);

        // ---- five-stage filter network + output trim --------------------
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            auto& hp = hpFilters[(size_t) ch];
            auto& lowShelf = lowShelfFilters[(size_t) ch];
            auto& bell = bellFilters[(size_t) ch];
            auto& highShelf = highShelfFilters[(size_t) ch];
            auto& lp = lpFilters[(size_t) ch];

            for (int i = 0; i < numSamples; ++i)
            {
                auto y = data[i];
                y = hp.processSample (y);
                y = lowShelf.processSample (y);
                y = bell.processSample (y);
                y = highShelf.processSample (y);
                y = lp.processSample (y);
                y *= outputTrimGainLinear;
                data[i] = y;
            }
        }

        // ---- enable/disable crossfade (sample-accurate) ------------------
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
