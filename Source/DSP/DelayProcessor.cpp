#include "DelayProcessor.h"

#include <algorithm>
#include <cmath>

namespace uni76::dsp
{
    void DelayProcessor::prepare (double sampleRateIn, int maximumBlockSize, int numChannelsToUse)
    {
        juce::ignoreUnused (maximumBlockSize);

        sampleRate = sampleRateIn;
        numChannels = juce::jlimit (1, 2, numChannelsToUse);

        // Sized for the safety-clamped worst case (see DelayCurves.h's
        // delayMaxTimeSeconds) plus a few samples of headroom for the
        // linear-interpolation read - a fixed allocation, sized once
        // here, never resized in process().
        bufferCapacitySamples = (int) std::ceil (delayMaxTimeSeconds * sampleRate) + 4;
        bufferL.assign ((size_t) bufferCapacitySamples, 0.0f);
        bufferR.assign ((size_t) bufferCapacitySamples, 0.0f);

        makeHighPassButterworth (feedbackHighpassL, sampleRate, delayFeedbackHighpassHz);
        makeHighPassButterworth (feedbackHighpassR, sampleRate, delayFeedbackHighpassHz);
        feedbackDampingL.setCutoffHz (sampleRate, delayFeedbackDampingHz);
        feedbackDampingR.setCutoffHz (sampleRate, delayFeedbackDampingHz);

        mixSmoother.reset (sampleRate, 0.02);
        feedbackSmoother.reset (sampleRate, 0.02);
        bypassSmoother.reset (sampleRate, 0.02);
        voiceCrossfade.reset (sampleRate, delayVoiceCrossfadeSeconds);

        // DELAY's default (0% mix) is its identity point - matches
        // ParameterLayout.cpp's own default so a freshly prepared
        // instance never ramps from a wrong value before the host's
        // first real parameter update arrives - same reasoning every
        // other module's prepare() already documents.
        mixSmoother.setCurrentAndTargetValue (0.0f);
        feedbackSmoother.setCurrentAndTargetValue (0.3f);
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        const auto defaultTimeSamples = (float) (delayTimeSecondsForDivision (delayDefaultDivisionIndex, delayFallbackBpm) * sampleRate);
        voiceTimeSamples = { defaultTimeSamples, defaultTimeSamples };
        voiceCrossfade.setCurrentAndTargetValue (0.0f);

        reset();
    }

    void DelayProcessor::reset() noexcept
    {
        std::fill (bufferL.begin(), bufferL.end(), 0.0f);
        std::fill (bufferR.begin(), bufferR.end(), 0.0f);
        writePosL = 0;
        writePosR = 0;

        feedbackHighpassL.reset();
        feedbackHighpassR.reset();
        feedbackDampingL.reset();
        feedbackDampingR.reset();
    }

    float DelayProcessor::readInterpolated (const std::vector<float>& buf, float readPosSamplesBack, int writePos) const noexcept
    {
        auto readPosF = (float) writePos - readPosSamplesBack;
        if (! std::isfinite (readPosF))
            readPosF = (float) writePos;
        readPosF = std::fmod (readPosF, (float) bufferCapacitySamples);
        if (readPosF < 0.0f)
            readPosF += (float) bufferCapacitySamples;

        const auto idx0 = juce::jlimit (0, bufferCapacitySamples - 1, (int) readPosF);
        const auto frac = juce::jlimit (0.0f, 1.0f, readPosF - (float) idx0);
        const auto idx1 = (idx0 + 1 == bufferCapacitySamples) ? 0 : idx0 + 1;
        return buf[(size_t) idx0] * (1.0f - frac) + buf[(size_t) idx1] * frac;
    }

    void DelayProcessor::process (juce::AudioBuffer<float>& buffer, float mixNormalised01, float feedbackNormalised01,
                                   int divisionIndex, bool stereo, bool pingPong, double hostBpm, bool enabled) noexcept
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

        const auto safeMix = std::isfinite (mixNormalised01) ? mixNormalised01 : 0.0f;
        mixSmoother.setTargetValue (std::clamp (safeMix, 0.0f, 1.0f));
        const auto safeFeedback = std::isfinite (feedbackNormalised01) ? feedbackNormalised01 : 0.0f;
        feedbackSmoother.setTargetValue (std::clamp (safeFeedback, 0.0f, 1.0f));
        bypassSmoother.setTargetValue (enabled ? 1.0f : 0.0f);

        // Per the product brief's own explicit conflict rule: PING PONG
        // being on always forces the *effective* mode to stereo/ping-pong
        // regardless of what the raw `stereo` flag currently holds (a DAW
        // automation lane, or a restored/hand-edited state, could
        // otherwise leave them in a "PING PONG on, MONO" combination) -
        // resolved here, every block, rather than by ever mutating either
        // parameter from this (the audio) thread.
        const bool stereoBus = channels >= 2 && numChannels >= 2;
        const bool effectivePingPong = pingPong && stereoBus;
        const bool effectiveStereo = (stereo || pingPong) && stereoBus;

        const auto tForCoefficients = mixSmoother.getCurrentValue();
        mixSmoother.skip (numSamples);
        const auto feedbackForCoefficients = feedbackSmoother.getCurrentValue();
        feedbackSmoother.skip (numSamples);
        const auto feedbackGain = feedbackForCoefficients * delayFeedbackMaxGain;

        // Tempo-synced time target (see DelayCurves.h) - resolved once
        // per block, same one-block-lag pattern every other module's own
        // macro-derived coefficients already use.
        const auto targetTimeSamples = (float) (delayTimeSecondsForDivision (divisionIndex, hostBpm) * sampleRate);

        // Click-free time changes: retarget the currently-inactive voice
        // and start a crossfade toward it, but only when no crossfade is
        // already in flight (see DelayProcessor.h's class comment) - a
        // rapid second change arriving mid-transition is picked up on the
        // next block once the current one settles, rather than
        // interrupting an in-progress fade.
        if (! voiceCrossfade.isSmoothing())
        {
            const auto settledIsZero = voiceCrossfade.getCurrentValue() < 0.5f;
            const auto inactiveSlot = settledIsZero ? 1 : 0;
            if (std::abs (targetTimeSamples - voiceTimeSamples[(size_t) inactiveSlot]) > 0.5f)
            {
                voiceTimeSamples[(size_t) inactiveSlot] = targetTimeSamples;
                voiceCrossfade.setTargetValue (settledIsZero ? 1.0f : 0.0f);
            }
        }

        const auto driveNorm = std::tanh (delayFeedbackDriveGain);

        const bool stereoInput = channels >= 2;
        auto* L = buffer.getWritePointer (0);
        auto* R = stereoInput ? buffer.getWritePointer (1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto mix = bypassSmoother.getNextValue();
            const auto cf = voiceCrossfade.getNextValue();

            const auto l = L[i];
            const auto r = stereoInput ? R[i] : l;
            const auto monoSend = 0.5f * (l + r);

            // ---- read both voices' constant-time taps, crossfaded -----
            const auto tapL0 = readInterpolated (bufferL, voiceTimeSamples[0], writePosL);
            const auto tapL1 = readInterpolated (bufferL, voiceTimeSamples[1], writePosL);
            const auto tapL = tapL0 * (1.0f - cf) + tapL1 * cf;

            const auto tapR0 = readInterpolated (bufferR, voiceTimeSamples[0], writePosR);
            const auto tapR1 = readInterpolated (bufferR, voiceTimeSamples[1], writePosR);
            const auto tapR = tapR0 * (1.0f - cf) + tapR1 * cf;

            // ---- feedback-path tone shaping (see DelayCurves.h) --------
            // Applied to EVERY read, not just what feeds back - this is
            // deliberately both the audible "repeat" character (warm,
            // progressively darker) and the safety-relevant bounded
            // waveshaper, at once. FEEDBACK itself only scales how much
            // of this tone-shaped tap re-enters the buffer below - the
            // audible tap is always at full level, which is what makes
            // FEEDBACK=0% a single full-volume repeat with no further
            // regeneration, not a quiet one.
            auto toneShapedL = feedbackHighpassL.processSample (tapL);
            toneShapedL = feedbackDampingL.processSample (toneShapedL);
            const auto xdL = toneShapedL * delayFeedbackDriveGain;
            toneShapedL = (xdL >= 0.0f ? std::tanh (xdL) : std::tanh (xdL * (1.0f - delayFeedbackAsymmetry))) / driveNorm;

            auto toneShapedR = feedbackHighpassR.processSample (tapR);
            toneShapedR = feedbackDampingR.processSample (toneShapedR);
            const auto xdR = toneShapedR * delayFeedbackDriveGain;
            toneShapedR = (xdR >= 0.0f ? std::tanh (xdR) : std::tanh (xdR * (1.0f - delayFeedbackAsymmetry))) / driveNorm;

            const auto feedbackContribL = toneShapedL * feedbackGain;
            const auto feedbackContribR = toneShapedR * feedbackGain;

            // ---- mode-dependent write routing (see class comment's
            // "Mode routing" section) -----------------------------------
            float writeL, writeR;
            float wetL, wetR;

            if (effectivePingPong)
            {
                // Classic single-chain ping-pong: input enters once, on
                // the Left side by design (see docs/DSP_DELAY.md) - bufferL
                // gets the mono send plus bufferR's own feedback tap;
                // bufferR gets ONLY bufferL's feedback tap, no direct
                // input at all. This is what makes a mono/centred source
                // alternate cleanly L-R-L-R rather than double-hitting
                // both sides at once.
                writeL = monoSend + feedbackContribR;
                writeR = feedbackContribL;
                wetL = toneShapedL;
                wetR = toneShapedR;
            }
            else if (effectiveStereo)
            {
                // Two fully independent feedback loops, one per channel -
                // no cross-talk, same BPM division on both (there is only
                // one pair of voice times in this whole class).
                writeL = l + feedbackContribL;
                writeR = r + feedbackContribR;
                wetL = toneShapedL;
                wetR = toneShapedR;
            }
            else
            {
                // MONO - a single self-feeding loop (bufferL only); both
                // output channels receive the identical wet tap, and the
                // wet send into it is the normalised mono sum (no gain
                // increase from summing) - bufferR is left untouched
                // (not written) so switching back to STEREO/PING-PONG
                // later doesn't start from stale content.
                writeL = monoSend + feedbackContribL;
                writeR = 0.0f;
                wetL = toneShapedL;
                wetR = toneShapedL;
            }

            bufferL[(size_t) writePosL] = writeL;
            writePosL = (writePosL + 1 == bufferCapacitySamples) ? 0 : writePosL + 1;

            if (effectiveStereo || effectivePingPong)
            {
                bufferR[(size_t) writePosR] = writeR;
                writePosR = (writePosR + 1 == bufferCapacitySamples) ? 0 : writePosR + 1;
            }

            // ---- MIX: a genuine dry/wet crossfade (see class comment) -
            // at mix==0 (MIX knob at 0%) or mix==0 (disabled), output is
            // exactly the unmodified dry signal; at mix==1 only the
            // processed tap is heard.
            const auto wetAmount = tForCoefficients * mix;
            L[i] = l * (1.0f - wetAmount) + wetL * wetAmount;
            if (stereoInput)
                R[i] = r * (1.0f - wetAmount) + wetR * wetAmount;
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
