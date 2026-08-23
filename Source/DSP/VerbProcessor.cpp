#include "VerbProcessor.h"

#include <algorithm>
#include <cmath>

namespace uni76::dsp
{
    namespace
    {
        // Fixed decorrelated stereo output-tap sign patterns for the 12
        // FDN lines - two different (not proportional to each other)
        // patterns, so L and R draw on the same tank but combine it
        // differently, the way a real plate's two physical pickups at
        // different positions would. See docs/DSP_VERB.md's "Plate
        // modal/FDN architecture" section for the measured decorrelation.
        constexpr std::array<float, verbNumLines> outputSignL { +1, +1, -1, -1, +1, +1, -1, -1, +1, +1, -1, -1 };
        constexpr std::array<float, verbNumLines> outputSignR { +1, -1, +1, -1, -1, +1, -1, +1, +1, -1, +1, -1 };
    }

    void VerbProcessor::prepare (double sampleRateIn, int maximumBlockSize, int numChannelsToUse)
    {
        juce::ignoreUnused (maximumBlockSize);

        sampleRate = sampleRateIn;
        numChannels = juce::jlimit (1, 2, numChannelsToUse);

        makeHighPassButterworth (wetSendHighpassA, sampleRate, verbWetSendHighpassHz);
        makeHighPassButterworth (wetSendHighpassB, sampleRate, verbWetSendHighpassHz);
        makeHighPassButterworth (wetOutputHighpassL, sampleRate, verbWetOutputHighpassHz);
        makeHighPassButterworth (wetOutputHighpassR, sampleRate, verbWetOutputHighpassHz);

        // Sized generously above the largest pre-delay anchor (20ms) for
        // headroom, plus a few samples for the linear-interpolation read.
        const auto preDelayCapacity = (int) std::ceil (0.04 * sampleRate) + 4;
        preDelayBuffer.assign ((size_t) preDelayCapacity, 0.0f);

        for (int j = 0; j < verbNumDiffusers; ++j)
        {
            const auto length = juce::jmax (1, (int) std::round (verbDiffuserLengthsMs[(size_t) j] * 0.001 * sampleRate));
            diffuserBuffers[(size_t) j].assign ((size_t) length, 0.0f);
        }

        for (int k = 0; k < verbNumLines; ++k)
        {
            const auto length = juce::jmax (1, (int) std::round (verbLineLengthsMs[(size_t) k] * 0.001 * sampleRate));
            lineLengthSamples[(size_t) k] = length;
            lineBuffers[(size_t) k].assign ((size_t) length, 0.0f);
            lineDamping[(size_t) k].setCutoffHz (sampleRate, verbDampingHz);
        }

        returnBandwidthL.setCutoffHz (sampleRate, verbReturnBandwidthHz);
        returnBandwidthR.setCutoffHz (sampleRate, verbReturnBandwidthHz);

        wetSmoother.reset (sampleRate, verbSmoothingSeconds);
        bypassSmoother.reset (sampleRate, verbSmoothingSeconds);
        // DRY (0%) is this module's default and identity point - matches
        // ParameterLayout.cpp's default so a freshly prepared instance
        // never ramps from a wrong value before the host's first real
        // parameter update arrives.
        wetSmoother.setCurrentAndTargetValue (0.0f);
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        reset();
        updateDecayDependentCoefficients (0.0f);
    }

    void VerbProcessor::reset() noexcept
    {
        wetSendHighpassA.reset();
        wetSendHighpassB.reset();
        wetOutputHighpassL.reset();
        wetOutputHighpassR.reset();

        std::fill (preDelayBuffer.begin(), preDelayBuffer.end(), 0.0f);
        preDelayWritePos = 0;

        for (int j = 0; j < verbNumDiffusers; ++j)
        {
            std::fill (diffuserBuffers[(size_t) j].begin(), diffuserBuffers[(size_t) j].end(), 0.0f);
            diffuserWritePos[(size_t) j] = 0;
        }

        for (int k = 0; k < verbNumLines; ++k)
        {
            std::fill (lineBuffers[(size_t) k].begin(), lineBuffers[(size_t) k].end(), 0.0f);
            lineWritePos[(size_t) k] = 0;
            lineDamping[(size_t) k].reset();
        }

        returnBandwidthL.reset();
        returnBandwidthR.reset();
    }

    void VerbProcessor::updateDecayDependentCoefficients (float wetForCoefficients) noexcept
    {
        // Classic feedback-gain-for-target-RT60 formula: after n passes
        // of a delay line L samples long, g^n == 10^(-60/20) (i.e. -60dB)
        // when n*L/sampleRate == RT60 seconds - solving for g gives
        // g = 10^(-3*(L/sampleRate)/RT60). Each line gets its own gain
        // since each has a different length; combined with the per-line
        // damping filter (frequency-dependent extra loss), this is what
        // produces a longer decay as the macro increases while keeping
        // the same plate character (see docs/DSP_VERB.md).
        const auto decaySeconds = juce::jmax (0.05f, verbDecaySeconds (wetForCoefficients));
        for (int k = 0; k < verbNumLines; ++k)
        {
            const auto lineSeconds = (float) lineLengthSamples[(size_t) k] / (float) sampleRate;
            const auto rawGain = std::pow (10.0f, -3.0f * lineSeconds / decaySeconds);
            // See verbLineFeedbackGainMax's comment (VerbCurves.h) - a
            // hard safety ceiling independent of the RT60 formula above.
            lineFeedbackGain[(size_t) k] = std::min (verbLineFeedbackGainMax, rawGain);
        }
    }

    void VerbProcessor::process (juce::AudioBuffer<float>& buffer, float wetNormalised01, bool enabled) noexcept
    {
        const auto numSamples = buffer.getNumSamples();
        const auto channels = buffer.getNumChannels();

        if (numSamples <= 0)
            return;

        // ---- numeric safety at the input boundary --------------------
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                if (! std::isfinite (data[i]))
                    data[i] = 0.0f;
        }

        // std::clamp does not clamp NaN (comparisons against NaN are
        // always false, so it returns the input unchanged) - an
        // std::isfinite guard is needed first, otherwise a non-finite
        // wetNormalised01 poisons wetSmoother's target/current value for
        // every future block until the next valid update, which then
        // reaches verbPiecewise()'s array-index computation (VerbCurves.h)
        // as a non-finite t01. See docs/DSP_VERB.md's "A real bug found by
        // Debug-mode testing" section - this is the same undefined-
        // behaviour class as the pre-delay index bug, caught at a second,
        // independent location by the same class of regression test.
        const auto safeWetNormalised01 = std::isfinite (wetNormalised01) ? wetNormalised01 : 0.0f;
        wetSmoother.setTargetValue (std::clamp (safeWetNormalised01, 0.0f, 1.0f));
        bypassSmoother.setTargetValue (enabled ? 1.0f : 0.0f);

        // Coefficients derived from the macro value are recomputed once
        // per block (from the smoothed value settled at the *start* of
        // this block), not per sample - same one-block-lag pattern
        // EqProcessor/SatProcessor already use: none of these (wet gain,
        // decay-derived feedback gain, pre-delay time) need audio-rate
        // precision the way PAN's LFO-driven rotation did, and this
        // avoids 12 pow() calls every single sample for no audible
        // benefit.
        const auto tForCoefficients = wetSmoother.getCurrentValue();
        const auto wetGain = verbWetGain (tForCoefficients);
        const auto preDelaySamples = verbPreDelayMs (tForCoefficients) * 0.001f * (float) sampleRate;
        updateDecayDependentCoefficients (tForCoefficients);
        wetSmoother.skip (numSamples);

        const bool stereo = channels >= 2 && numChannels >= 2;
        auto* L = buffer.getWritePointer (0);
        auto* R = stereo ? buffer.getWritePointer (1) : nullptr;

        const auto preDelayCapacity = (int) preDelayBuffer.size();
        constexpr float sqrtNumLines = 3.4641016151377544f; // sqrt(12)
        constexpr float houseworthScale = 2.0f / (float) verbNumLines;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto mix = bypassSmoother.getNextValue();

            const auto l = L[i];
            const auto r = stereo ? R[i] : l;
            const auto monoSum = 0.5f * (l + r);

            // ---- wet send HPF (4-pole, 2x cascaded 2-pole) ----------
            auto sent = wetSendHighpassA.processSample (monoSum);
            sent = wetSendHighpassB.processSample (sent);

            // ---- analog send stage (tiny asymmetric tanh) -----------
            // Same bounded per-half-gain tanh() shape PREAMP/SAT use,
            // own fixed (much smaller) constants - see VerbCurves.h.
            const auto sendXd = sent * verbSendDriveGain;
            const auto sendShaped = sendXd >= 0.0f ? std::tanh (sendXd) : std::tanh (sendXd * (1.0f - verbSendAsymmetry));
            const auto sendNorm = std::tanh (verbSendDriveGain);
            const auto sentShaped = sendShaped / sendNorm;

            // ---- diffuser (4-stage short-delay Schroeder allpass) ---
            // Early-density stage ahead of the FDN tank - NOT used alone
            // as the whole reverb (that would be the "cheap Schroeder"
            // architecture explicitly rejected - see docs/DSP_VERB.md).
            auto diffused = sentShaped;
            for (int j = 0; j < verbNumDiffusers; ++j)
            {
                auto& buf = diffuserBuffers[(size_t) j];
                auto& pos = diffuserWritePos[(size_t) j];
                const auto delayed = buf[(size_t) pos];
                const auto y = -verbDiffuserGain * diffused + delayed;
                buf[(size_t) pos] = diffused + verbDiffuserGain * y;
                pos = (pos + 1 == (int) buf.size()) ? 0 : pos + 1;
                diffused = y;
            }

            // ---- pre-delay (smoothly variable, linear-interpolated) --
            preDelayBuffer[(size_t) preDelayWritePos] = diffused;
            // std::fmod (not a manual while-loop-until-positive) is used
            // for the wraparound specifically because it terminates for
            // any finite input in one step - the previous while-loop
            // form could in principle spin or leave an out-of-range
            // value behind for a non-finite `readPosF` (NaN compares
            // false against 0.0f either way, so `while (readPosF <
            // 0.0f)` would never execute and (int) of a NaN is undefined
            // behaviour) - defended against directly here too via
            // isfinite, since this feeds a raw buffer index.
            auto readPosF = (float) preDelayWritePos - preDelaySamples;
            if (! std::isfinite (readPosF))
                readPosF = (float) preDelayWritePos;
            readPosF = std::fmod (readPosF, (float) preDelayCapacity);
            if (readPosF < 0.0f)
                readPosF += (float) preDelayCapacity;
            const auto readIndex0 = juce::jlimit (0, preDelayCapacity - 1, (int) readPosF);
            const auto frac = juce::jlimit (0.0f, 1.0f, readPosF - (float) readIndex0);
            const auto readIndex1 = (readIndex0 + 1 == preDelayCapacity) ? 0 : readIndex0 + 1;
            const auto preDelayed = preDelayBuffer[(size_t) readIndex0] * (1.0f - frac)
                                   + preDelayBuffer[(size_t) readIndex1] * frac;
            preDelayWritePos = (preDelayWritePos + 1 == preDelayCapacity) ? 0 : preDelayWritePos + 1;

            // ---- FDN plate tank --------------------------------------
            std::array<float, verbNumLines> rawLineOut {};
            for (int k = 0; k < verbNumLines; ++k)
                rawLineOut[(size_t) k] = lineBuffers[(size_t) k][(size_t) lineWritePos[(size_t) k]];

            std::array<float, verbNumLines> dampedFeedback {};
            float feedbackSum = 0.0f;
            for (int k = 0; k < verbNumLines; ++k)
            {
                const auto damped = lineDamping[(size_t) k].processSample (rawLineOut[(size_t) k]);
                dampedFeedback[(size_t) k] = damped * lineFeedbackGain[(size_t) k];
                feedbackSum += dampedFeedback[(size_t) k];
            }

            // Householder reflection using the all-ones vector - an
            // energy-preserving (orthogonal) feedback matrix computable
            // in O(N) rather than a full N*N multiply: mixed = fb - (2/N)
            // * (sum of fb) * ones. See docs/DSP_VERB.md.
            const auto houseTerm = houseworthScale * feedbackSum;
            for (int k = 0; k < verbNumLines; ++k)
            {
                auto& buf = lineBuffers[(size_t) k];
                auto& pos = lineWritePos[(size_t) k];
                buf[(size_t) pos] = preDelayed + (dampedFeedback[(size_t) k] - houseTerm);
                pos = (pos + 1 == (int) buf.size()) ? 0 : pos + 1;
            }

            // ---- decorrelated stereo output taps ---------------------
            float tapL = 0.0f, tapR = 0.0f;
            for (int k = 0; k < verbNumLines; ++k)
            {
                tapL += rawLineOut[(size_t) k] * outputSignL[(size_t) k];
                tapR += rawLineOut[(size_t) k] * outputSignR[(size_t) k];
            }
            tapL /= sqrtNumLines;
            tapR /= sqrtNumLines;

            // ---- analog return stage (tiny tanh + bandwidth ceiling) -
            const auto retXdL = tapL * verbReturnDriveGain;
            const auto retShapedL = retXdL >= 0.0f ? std::tanh (retXdL) : std::tanh (retXdL * (1.0f - verbReturnAsymmetry));
            const auto retNorm = std::tanh (verbReturnDriveGain);
            const auto returnedL = returnBandwidthL.processSample (retShapedL / retNorm);

            const auto retXdR = tapR * verbReturnDriveGain;
            const auto retShapedR = retXdR >= 0.0f ? std::tanh (retXdR) : std::tanh (retXdR * (1.0f - verbReturnAsymmetry));
            const auto returnedR = returnBandwidthR.processSample (retShapedR / retNorm);

            // ---- wet output HPF safety (lighter, 2-pole) -------------
            const auto safeL = wetOutputHighpassL.processSample (returnedL);
            const auto safeR = wetOutputHighpassR.processSample (returnedR);

            // Dry is read into locals above and written back here,
            // unmodified - the wet contribution is purely additive and
            // scaled by wetGain (an aux-send level, not a crossfade) and
            // the bypass smoother. At wetGain==0 (t=0/DRY) or mix==0
            // (disabled), the wet term is exactly zero regardless of the
            // plate tank's internal state - see the class comment in
            // VerbProcessor.h.
            const auto wetContribution = wetGain * mix;
            L[i] = l + wetContribution * safeL;
            if (stereo)
                R[i] = r + wetContribution * safeR;
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
