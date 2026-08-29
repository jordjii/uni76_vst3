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
        eqSmoother.setCurrentAndTargetValue (0.5f); // matches the "eq" parameter's own 50% (CENTER) default
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        reset();
        updateCoefficients (0.5f);
    }

    void EqProcessor::reset() noexcept
    {
        for (int ch = 0; ch < maxChannels; ++ch)
        {
            for (auto& stage : hpStages[(size_t) ch]) stage.reset();
            for (auto& stage : lpStages[(size_t) ch]) stage.reset();
        }
    }

    void EqProcessor::updateCoefficients (float eqForCoefficients) noexcept
    {
        const auto params = eqParamsAt (eqForCoefficients);

        // Clamp to a safe fraction of Nyquist so the cascade stays stable
        // and musically meaningful at every supported sample rate - the
        // highest anchor's ~11.9kHz LP target, for instance, would sit
        // uncomfortably close to Nyquist at 44.1kHz without this.
        const auto nyquist = (float) (sampleRate * 0.5);
        const auto safeMax = nyquist * 0.9f;
        const auto hpHz = juce::jlimit (5.0f, safeMax, params.hpHz);
        const auto lpHz = juce::jlimit (20.0f, safeMax, params.lpHz);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            for (auto& stage : hpStages[(size_t) ch])
                makeHighPassQ (stage, sampleRate, hpHz, params.hpQ);

            for (auto& stage : lpStages[(size_t) ch])
                makeLowPassQ (stage, sampleRate, lpHz, params.lpQ);
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

        // ---- two-cut (48dB/oct HP + 48dB/oct LP) filter network ---------
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            auto& hp = hpStages[(size_t) ch];
            auto& lp = lpStages[(size_t) ch];

            for (int i = 0; i < numSamples; ++i)
            {
                auto y = data[i];
                for (auto& stage : hp) y = stage.processSample (y);
                for (auto& stage : lp) y = stage.processSample (y);
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
