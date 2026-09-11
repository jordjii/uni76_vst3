#include "SatProcessor.h"
#include "SatCurves.h"

#include <cmath>

namespace uni76::dsp
{
    int SatProcessor::chooseOversamplingStages (double sr) noexcept
    {
        // Same policy as PreampProcessor (see docs/DSP_SAT.md for why a
        // second, independent oversampling instance is the right call
        // here rather than reworking PREAMP's to be "shared" - PREAMP's
        // sound is frozen and not to be touched for code-reuse reasons):
        // 44.1/48kHz -> 4x, 88.2/96kHz -> 2x, 176.4/192kHz+ -> none.
        if (sr <= 48000.0 + 1.0)
            return 2;
        if (sr <= 96000.0 + 1.0)
            return 1;
        return 0;
    }

    void SatProcessor::prepare (double sampleRateIn, int maximumBlockSize, int numChannelsToUse)
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
                true,   // isMaximumQuality
                true);  // useIntegerLatency

            oversampler->initProcessing ((size_t) maximumBlockSize);
            latencySamples = (int) oversampler->getLatencyInSamples();
        }
        else
        {
            oversampler.reset();
            latencySamples = 0;
        }

        maxOversampledBlockSize = maximumBlockSize * factor;

        heatRampScratch.assign ((size_t) maxOversampledBlockSize, 0.0f);
        bypassRampScratch.assign ((size_t) maximumBlockSize, 0.0f);

        dryScratch.setSize (numChannels, maximumBlockSize, false, false, true);

        heatSmoother.reset (oversampledRate, satDriveSmoothingSeconds);
        bypassSmoother.reset (sampleRate, satBypassSmoothingSeconds);
        heatSmoother.setCurrentAndTargetValue (0.0f);
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            dcBlockers[(size_t) ch].setCutoffHz (sampleRate, satDcBlockerHz);
            outputDcBlockers[(size_t) ch].setCutoffHz (sampleRate, satDcBlockerHz);
            dryDelays[(size_t) ch].prepare (latencySamples);
            envelopeFollowers[(size_t) ch].setReleaseMs (oversampledRate, satEnvelopeReleaseMs);
        }

        reset();
        updateBlockCoefficients (0.0f);
    }

    void SatProcessor::reset() noexcept
    {
        for (int ch = 0; ch < maxChannels; ++ch)
        {
            dcBlockers[(size_t) ch].reset();
            outputDcBlockers[(size_t) ch].reset();
            lowShelfPre[(size_t) ch].reset();
            highShelfPre[(size_t) ch].reset();
            highShelfDe[(size_t) ch].reset();
            lowShelfDe[(size_t) ch].reset();
            envelopeFollowers[(size_t) ch].reset();
            dryDelays[(size_t) ch].reset();
        }

        if (oversampler != nullptr)
            oversampler->reset();
    }

    void SatProcessor::updateBlockCoefficients (float heatForCoefficients) noexcept
    {
        const auto t = satClamp01 (heatForCoefficients);

        const auto lowDb  = satLowShelfGainDb (t);
        const auto highDb = satHighShelfGainDb (t);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            // Pre-emphasis: low-shelf cut then high-shelf boost.
            makeLowShelf  (lowShelfPre[(size_t) ch],  oversampledRate, satLowShelfFreqHz,  lowDb);
            makeHighShelf (highShelfPre[(size_t) ch], oversampledRate, satHighShelfFreqHz, highDb);

            // De-emphasis: exact algebraic inverse, reverse order
            // ((A then B)^-1 = B^-1 then A^-1) - high-shelf cut then
            // low-shelf boost.
            makeHighShelf (highShelfDe[(size_t) ch], oversampledRate, satHighShelfFreqHz, -highDb);
            makeLowShelf  (lowShelfDe[(size_t) ch],  oversampledRate, satLowShelfFreqHz,  -lowDb);
        }
    }

    void SatProcessor::process (juce::AudioBuffer<float>& buffer, float heatNormalised01, bool enabled) noexcept
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
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* dry = dryScratch.getWritePointer (ch);
            const auto* in = buffer.getReadPointer (ch);

            for (int i = 0; i < numSamples; ++i)
                dry[i] = dryDelays[(size_t) ch].processSample (in[i]);
        }

        // ---- smoothing targets -----------------------------------------
        const auto heatForCoefficients = heatSmoother.getCurrentValue();
        heatSmoother.setTargetValue (satClamp01 (heatNormalised01));
        bypassSmoother.setTargetValue (enabled ? 1.0f : 0.0f);

        updateBlockCoefficients (heatForCoefficients);

        // ---- DC protection (base rate, fixed) --------------------------
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
            heatRampScratch[(size_t) i] = heatSmoother.getNextValue();

        // ---- tilt pre-emphasis -> dynamic gain -> waveshaper -> tilt de-emphasis (oversampled) --
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = processingBlock.getChannelPointer ((size_t) ch);
            auto& lowPre  = lowShelfPre[(size_t) ch];
            auto& highPre = highShelfPre[(size_t) ch];
            auto& highDe  = highShelfDe[(size_t) ch];
            auto& lowDe   = lowShelfDe[(size_t) ch];
            auto& envelope = envelopeFollowers[(size_t) ch];

            for (int i = 0; i < oversampledNumSamples; ++i)
            {
                const auto t = heatRampScratch[(size_t) i];

                auto x = data[i];
                x = lowPre.processSample (x);
                x = highPre.processSample (x);

                // Energy-dependent dynamic gain: bounded in (0, 1] for any
                // non-negative envelope - a soft "glue" compressor ahead
                // of the waveshaper, the part that makes SAT genuinely
                // different from PREAMP's static per-sample model.
                const auto env = envelope.processSample (x);
                const auto compressionStrength = satCompressionStrength (t);
                const auto gainReduction = 1.0f / (1.0f + compressionStrength * env);
                x *= gainReduction;

                const auto driveGain = satDriveGainLinear (t);
                const auto xd = x * driveGain;

                // Same bounded per-half-gain asymmetry PREAMP's
                // calibration pass settled on - see docs/DSP_PREAMP.md's
                // "What changed in the nonlinear model" for why an
                // unbounded additive term is never used here.
                const auto asym = satAsymmetryAmount (t);
                const auto shaped = xd >= 0.0f ? std::tanh (xd) : std::tanh (xd * (1.0f - asym));

                const auto norm = std::tanh (driveGain);
                auto y = norm > 1.0e-6f ? shaped / norm : shaped;

                y = highDe.processSample (y);
                y = lowDe.processSample (y);

                data[i] = y;
            }
        }

        // ---- downsample -----------------------------------------------
        if (oversampler != nullptr)
            oversampler->processSamplesDown (block);

        // ---- remove the DC the asymmetric waveshaper itself generated --
        // (see SatProcessor.h's outputDcBlockers comment - the input-side
        // blocker above cannot do this). At base rate: DC is DC, there's
        // nothing to gain from running it oversampled.
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                data[i] = outputDcBlockers[(size_t) ch].processSample (data[i]);
        }

        // ---- output compensation (drive-dependent trim) ----------------
        const auto factor = oversamplingStages > 0 ? (1 << oversamplingStages) : 1;
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                const auto rampIndex = juce::jlimit (0, oversampledNumSamples - 1, i * factor);
                const auto t = heatRampScratch[(size_t) rampIndex];
                data[i] *= juce::Decibels::decibelsToGain (satOutputCompensationDb (t));
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
