#include "VerbProcessor.h"

#include <algorithm>
#include <cmath>

namespace uni76::dsp
{
    namespace
    {
        // Fixed decorrelated stereo output-tap sign patterns for the 16
        // tank lines - two different (not proportional to each other)
        // patterns, so L and R draw on the same tank but combine it
        // differently, the way two physically separate pickups/mics on a
        // real chamber would. See docs/DSP_VERB.md.
        constexpr std::array<float, verbNumLines> outputSignL
            { +1, +1, -1, +1, -1, -1, +1, -1, +1, +1, -1, -1, +1, -1, -1, +1 };
        constexpr std::array<float, verbNumLines> outputSignR
            { +1, -1, +1, +1, -1, +1, -1, -1, +1, -1, +1, -1, -1, -1, +1, +1 };

        /** Householder reflection using the all-ones vector - an
            energy-preserving (orthogonal) feedback matrix, computable in
            O(N) rather than a full N*N multiply: mixed = fb - (2/N) *
            (sum of fb) * ones. A generic FDN mixing primitive (Stautner &
            Puckette 1982), not specific to any one reverb design - what
            makes this tank a from-scratch redesign rather than a repair
            of the old plate module is its line count/lengths, its
            damping/decay tuning, and its bounded read-position modulation
            (see VerbCurves.h's "Tank line modulation" section), not this
            matrix choice. */
        inline void houseworthMix (std::array<float, verbNumLines>& v) noexcept
        {
            float sum = 0.0f;
            for (auto x : v) sum += x;
            constexpr float scale = 2.0f / (float) verbNumLines;
            const auto term = scale * sum;
            for (auto& x : v) x -= term;
        }
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

        // Fixed pre-delay - constant regardless of Mix (see VerbCurves.h's
        // "Decay/Pre-delay - FIXED" reasoning).
        preDelay.prepare (juce::jmax (1, (int) std::round (verbPreDelayMsValue * 0.001 * sampleRate)));

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
            lineModIncrement[(size_t) k] = twoPi * (double) verbLineModRateHz[(size_t) k] / sampleRate;
        }

        returnBandwidthL.setCutoffHz (sampleRate, verbReturnBandwidthHz);
        returnBandwidthR.setCutoffHz (sampleRate, verbReturnBandwidthHz);

        driveDepthLowpassL.setCutoffHz (sampleRate, verbReturnBandwidthHz);
        driveDepthLowpassR.setCutoffHz (sampleRate, verbReturnBandwidthHz);

        driveWidthDelayR.prepare (juce::jmax (1, (int) std::round (verbDriveWidthDelayMs * 0.001 * sampleRate)));

        breakupLevelAlpha = 1.0f - std::exp ((float) (-1.0 / (verbBreakupLevelReleaseSeconds * sampleRate)));
        breakupPeakDecayPerSample = (float) std::exp (-1.0 / (verbBreakupPeakReleaseSeconds * sampleRate));

        wetSmoother.reset (sampleRate, verbSmoothingSeconds);
        driveSmoother.reset (sampleRate, verbSmoothingSeconds);
        bypassSmoother.reset (sampleRate, verbSmoothingSeconds);
        // DRY (0%) is this module's default and identity point - matches
        // ParameterLayout.cpp's default so a freshly prepared instance
        // never ramps from a wrong value before the host's first real
        // parameter update arrives. DRIVE also starts at its own default
        // (0%, the original fixed coloration) for the same reason.
        wetSmoother.setCurrentAndTargetValue (0.0f);
        driveSmoother.setCurrentAndTargetValue (0.0f);
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        // Decay is now a FIXED target, completely independent of Mix - so,
        // unlike the old plate module, the per-line feedback gain only
        // needs computing once, here, not every block. Solved against
        // verbDecayFormulaTargetSeconds (an internal-only, measurement-
        // calibrated value, larger than the reported verbTargetDecaySeconds)
        // - see VerbCurves.h's "RT60-formula target" comment for why the
        // formula's own input has to be inflated to make the *measured*
        // decay land at the real ~2.8-3.2s spec.
        for (int k = 0; k < verbNumLines; ++k)
        {
            const auto lineSeconds = (float) lineLengthSamples[(size_t) k] / (float) sampleRate;
            const auto rawGain = std::pow (10.0f, -3.0f * lineSeconds / verbDecayFormulaTargetSeconds);
            lineFeedbackGain[(size_t) k] = std::min (verbLineFeedbackGainMax, rawGain);
        }

        reset();
    }

    void VerbProcessor::reset() noexcept
    {
        wetSendHighpassA.reset();
        wetSendHighpassB.reset();
        wetOutputHighpassL.reset();
        wetOutputHighpassR.reset();

        preDelay.reset();

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
            lineInterpolators[(size_t) k].reset();
            // Staggered starting phases (not just staggered rates) so
            // the 12 lines' own modulation never all cross zero
            // together, even for the first cycle right after a reset.
            lineModPhase[(size_t) k] = twoPi * (double) k / (double) verbNumLines;
        }

        returnBandwidthL.reset();
        returnBandwidthR.reset();

        returnWarmthShelfL.reset();
        returnWarmthShelfR.reset();

        driveDepthLowpassL.reset();
        driveDepthLowpassR.reset();

        driveWidthDelayR.reset();

        breakupLevelSmoothed = 0.0f;
        breakupPeakLevel = 0.0f;
    }

    void VerbProcessor::process (juce::AudioBuffer<float>& buffer, float wetNormalised01, float driveNormalised01, bool enabled) noexcept
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
        // as a non-finite t01. See docs/DSP_VERB.md's historical "A real
        // bug found by Debug-mode testing" section - this is the same
        // undefined-behaviour class the pre-delay index bug used to be,
        // guarded against directly here (and now moot for pre-delay
        // itself, since pre-delay is a fixed IntegerDelayLine with no
        // runtime index arithmetic left to poison).
        const auto safeWetNormalised01 = std::isfinite (wetNormalised01) ? wetNormalised01 : 0.0f;
        wetSmoother.setTargetValue (std::clamp (safeWetNormalised01, 0.0f, 1.0f));
        // DRIVE (nested knob) - same NaN-guard reasoning as the wet macro
        // just above, since this also reaches a smoother target.
        const auto safeDriveNormalised01 = std::isfinite (driveNormalised01) ? driveNormalised01 : 0.0f;
        driveSmoother.setTargetValue (std::clamp (safeDriveNormalised01, 0.0f, 1.0f));
        bypassSmoother.setTargetValue (enabled ? 1.0f : 0.0f);

        // Only the wet gain (Mix - dry/wet balance) is macro-dependent now
        // (see VerbCurves.h's "Decay/Pre-delay - FIXED" reasoning) - no
        // per-block RT60/pre-delay recomputation is needed any more.
        const auto tForCoefficients = wetSmoother.getCurrentValue();
        const auto wetGain = verbWetGain (tForCoefficients);
        wetSmoother.skip (numSamples);

        // DRIVE-derived send/return coefficients - once-per-block pattern
        // (no audio-rate precision needed - DRIVE is a slow user/
        // automation macro, not an LFO-driven value the way PAN's
        // rotation is).
        const auto driveForCoefficients = driveSmoother.getCurrentValue();
        const auto returnAsymmetry = verbReturnAsymmetry (driveForCoefficients);

        // The send stage never reads DRIVE at all - it is permanently
        // pinned to its own base ("texture") values, so the diffuser and
        // the tank always receive an essentially clean signal and produce
        // a clean, warm tail no matter where DRIVE sits.
        constexpr auto sendDriveGain = verbSendDriveGainBase;
        constexpr auto sendAsymmetry = verbSendAsymmetryBase;
        const auto sendDriveNorm = std::tanh (sendDriveGain);

        // DRIVE warmth (see VerbCurves.h's "DRIVE warmth" section) - a
        // low-shelf boost ahead of the return-stage tanh, scaled by the
        // raw DRIVE knob position (not envelope-gated - this is a tonal
        // voicing, not a level-triggered dynamic effect). At
        // driveForCoefficients==0 the gain is exactly 0dB, so makeLowShelf
        // collapses to an identity filter.
        const auto driveCurve = std::pow (std::clamp (driveForCoefficients, 0.0f, 1.0f), verbDriveCurveExponent);
        const auto warmthGainDb = verbReturnWarmthMaxDb * driveCurve;
        makeLowShelf (returnWarmthShelfL, sampleRate, verbReturnWarmthShelfHz, warmthGainDb);
        makeLowShelf (returnWarmthShelfR, sampleRate, verbReturnWarmthShelfHz, warmthGainDb);

        // DRIVE depth/width (see VerbCurves.h's "Driven-tail placement"
        // section) - the bandwidth ceiling walks down from the module's
        // own fixed value toward verbDriveDepthBandwidthMinHz as DRIVE
        // rises, so added harmonics never read as forward/"in your face";
        // the width mix (a FIXED, non-modulated R-channel offset - no
        // chorus/LFO anywhere in this path) rises with it. Both are
        // exactly their no-op values at DRIVE=0% (full bandwidth, zero
        // mix).
        const auto depthCutoffHz = verbReturnBandwidthHz
                                       + (verbDriveDepthBandwidthMinHz - verbReturnBandwidthHz) * driveCurve;
        driveDepthLowpassL.setCutoffHz (sampleRate, depthCutoffHz);
        driveDepthLowpassR.setCutoffHz (sampleRate, depthCutoffHz);

        const auto widthMix = verbDriveWidthMixMax * driveCurve;

        // Breakup boost (see VerbCurves.h's "Breakup" section) - reads the
        // level/peak ratio settled at the *end of the previous block*
        // (same one-block-lag pattern as every other coefficient here),
        // so a decaying tail's own return-stage drive gradually rises as
        // the tail fades relative to its own recent peak, then resets
        // once a new, louder transient re-anchors that peak. Scaled by
        // the raw DRIVE knob position (not verbReturnDriveGain itself) so
        // DRIVE=0% is a true no-op regardless of the level/peak ratio.
        const auto breakupRatio = breakupPeakLevel > 1.0e-6f
                                       ? juce::jlimit (0.0f, 1.0f, breakupLevelSmoothed / breakupPeakLevel)
                                       : 1.0f;
        const auto breakupBoost = verbBreakupAmount * driveForCoefficients * (1.0f - breakupRatio);
        const auto effReturnDriveGain = verbReturnDriveGain (driveForCoefficients) * (1.0f + breakupBoost);
        const auto effReturnDriveNorm = std::tanh (effReturnDriveGain);
        driveSmoother.skip (numSamples);

        // Derived, not hardcoded - see docs/DSP_VERB.md's own documented
        // history of a hardcoded sqrt(N) literal silently mis-scaling the
        // tank's output level the moment the line count changed.
        const auto sqrtNumLines = std::sqrt ((float) verbNumLines);

        const bool stereo = channels >= 2 && numChannels >= 2;
        auto* L = buffer.getWritePointer (0);
        auto* R = stereo ? buffer.getWritePointer (1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto mix = bypassSmoother.getNextValue();

            const auto l = L[i];
            const auto r = stereo ? R[i] : l;
            const auto monoSum = 0.5f * (l + r);
            // Real incoming stereo width (e.g. from PAN, if it runs before
            // VERB in the chain order) - the tank itself is fed from
            // monoSum alone, but this is carried through separately into
            // the wet output below so it isn't silently discarded - see
            // VerbCurves.h's "Input stereo-width carry-through" section.
            const auto side = 0.5f * (l - r);

            // ---- wet send HPF (4-pole, 2x cascaded 2-pole) ----------
            auto sent = wetSendHighpassA.processSample (monoSum);
            sent = wetSendHighpassB.processSample (sent);

            // ---- analog send stage (tiny asymmetric tanh, fixed) --------
            // Same bounded per-half-gain tanh() shape PREAMP/SAT use, own
            // (much smaller) values - see VerbCurves.h. Deliberately NOT
            // DRIVE-scaled: the tank must always receive a clean signal so
            // its tail stays warm rather than reverberating an already-
            // distorted input.
            const auto sendXd = sent * sendDriveGain;
            const auto sendShaped = sendXd >= 0.0f ? std::tanh (sendXd) : std::tanh (sendXd * (1.0f - sendAsymmetry));
            const auto sentShaped = sendShaped / sendDriveNorm;

            // ---- input diffusion cascade (8-stage short-delay Schroeder
            // allpass) - smooths the input into a dense wash BEFORE the
            // tank, so there is no discrete early-reflection "slap" and no
            // audible comb structure once the tank starts recirculating.
            // NOT used alone as the whole reverb (that would be the
            // "cheap Schroeder" architecture both the old and new product
            // briefs reject) - a pre-density stage feeding a proper tank.
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

            // ---- fixed pre-delay (constant - Mix never stretches this) --
            const auto preDelayed = preDelay.processSample (diffused);

            // ---- reverb tank: small, bounded, per-line-modulated reads --
            // See VerbCurves.h's "Tank line modulation" section for why
            // this exists at all (it is the actual anti-metallic
            // mechanism - a static FDN's modes are fixed for the life of
            // the instance, which is what reads as "metallic"/"a fixed
            // resonant note") and why the depth is kept far below the
            // threshold where it would read as an audible chorus/pitch
            // effect. At modulation depth 0 this collapses to exactly a
            // direct index read (frac==0, idx0==lineWritePos).
            std::array<float, verbNumLines> rawLineOut {};
            for (int k = 0; k < verbNumLines; ++k)
            {
                const auto& buf = lineBuffers[(size_t) k];
                const auto size = (int) buf.size();
                const auto modOffset = verbLineModDepthSamples * (float) std::sin (lineModPhase[(size_t) k]);

                auto readPosF = (float) lineWritePos[(size_t) k] + modOffset;
                if (! std::isfinite (readPosF))
                    readPosF = (float) lineWritePos[(size_t) k];
                readPosF = std::fmod (readPosF, (float) size);
                if (readPosF < 0.0f)
                    readPosF += (float) size;
                const auto idx0 = juce::jlimit (0, size - 1, (int) readPosF);
                const auto frac = juce::jlimit (0.0f, 1.0f, readPosF - (float) idx0);
                // AllpassFractionalDelay (Biquad.h), not plain linear
                // interpolation - a true allpass (flat magnitude response
                // at any fractional delay), so this modulation costs no
                // measurable per-pass loss inside the feedback loop, unlike
                // the frequency-dependent attenuation a 2-tap linear blend
                // would introduce here. See VerbCurves.h's "Tank line
                // modulation" section.
                rawLineOut[(size_t) k] = lineInterpolators[(size_t) k].processSample (buf[(size_t) idx0], frac);

                lineModPhase[(size_t) k] += lineModIncrement[(size_t) k];
                if (lineModPhase[(size_t) k] >= twoPi)
                    lineModPhase[(size_t) k] -= twoPi;
            }

            std::array<float, verbNumLines> mixedFeedback {};
            for (int k = 0; k < verbNumLines; ++k)
                mixedFeedback[(size_t) k] = lineDamping[(size_t) k].processSample (rawLineOut[(size_t) k]) * lineFeedbackGain[(size_t) k];

            // Householder reflection - an energy-preserving (orthogonal)
            // feedback matrix, O(N) - see the file-local houseworthMix()
            // helper above and VerbCurves.h's "Reverb tank" section.
            houseworthMix (mixedFeedback);

            for (int k = 0; k < verbNumLines; ++k)
            {
                auto& buf = lineBuffers[(size_t) k];
                auto& pos = lineWritePos[(size_t) k];
                buf[(size_t) pos] = preDelayed + mixedFeedback[(size_t) k];
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

            // ---- breakup envelope tracking (see VerbCurves.h's "Breakup"
            // section) - updates the state this block's own returnDriveGain
            // was computed from at the *start* of process() (one-block lag,
            // same as every other coefficient here), for use on the *next*
            // block.
            const auto breakupInstantLevel = 0.5f * (std::abs (tapL) + std::abs (tapR));
            if (std::isfinite (breakupInstantLevel))
            {
                breakupLevelSmoothed += breakupLevelAlpha * (breakupInstantLevel - breakupLevelSmoothed);
                breakupPeakLevel = std::max (breakupLevelSmoothed, breakupPeakLevel * breakupPeakDecayPerSample);
            }

            // ---- DRIVE warmth pre-emphasis (see VerbCurves.h's "DRIVE
            // warmth" section) - a low-shelf boost ahead of the tanh below,
            // biasing what reaches the nonlinearity toward low-mid content.
            // Identity at DRIVE=0% (warmthGainDb==0dB exactly).
            const auto warmedTapL = returnWarmthShelfL.processSample (tapL);
            const auto warmedTapR = returnWarmthShelfR.processSample (tapR);

            // ---- analog return stage (tiny tanh + bandwidth ceiling, DRIVE-scaled) --
            const auto retXdL = warmedTapL * effReturnDriveGain;
            const auto retShapedL = retXdL >= 0.0f ? std::tanh (retXdL) : std::tanh (retXdL * (1.0f - returnAsymmetry));
            auto returnedL = returnBandwidthL.processSample (retShapedL / effReturnDriveNorm);

            const auto retXdR = warmedTapR * effReturnDriveGain;
            const auto retShapedR = retXdR >= 0.0f ? std::tanh (retXdR) : std::tanh (retXdR * (1.0f - returnAsymmetry));
            auto returnedR = returnBandwidthR.processSample (retShapedR / effReturnDriveNorm);

            // ---- driven-tail depth (see VerbCurves.h's "Driven-tail
            // placement" section) - a second, DRIVE-dependent bandwidth
            // ceiling on top of the fixed one above, walking down as DRIVE
            // rises so the harmonics the waveshaper just added read as
            // going away into depth rather than forward. At DRIVE=0% its
            // cutoff equals verbReturnBandwidthHz, i.e. it is a second
            // pass of an already-applied ceiling and changes essentially
            // nothing.
            returnedL = driveDepthLowpassL.processSample (returnedL);
            returnedR = driveDepthLowpassR.processSample (returnedR);

            // ---- driven-tail width (FIXED, non-modulated inter-channel
            // offset) - blends a small, static delay of R into R itself,
            // proportional to DRIVE. No LFO anywhere in this path, unlike
            // the old plate module's driven-tail chorus - see
            // VerbCurves.h's "Driven-tail placement" section.
            {
                const auto delayedR = driveWidthDelayR.processSample (returnedR);
                returnedR += widthMix * (delayedR - returnedR);
            }

            // ---- wet output HPF safety (lighter, 2-pole) -------------
            const auto safeL = wetOutputHighpassL.processSample (returnedL);
            const auto safeR = wetOutputHighpassR.processSample (returnedR);

            // Dry is read into locals above and written back here,
            // unmodified - the wet contribution is purely additive and
            // scaled by wetGain (an aux-send level, not a crossfade) and
            // the bypass smoother. At wetGain==0 (t=0/DRY) or mix==0
            // (disabled), the wet term is exactly zero regardless of the
            // tank's internal state - see the class comment in
            // VerbProcessor.h.
            const auto wetContribution = wetGain * mix;
            // sideCarry preserves incoming stereo width in the wet output
            // itself (see the `side` comment above and VerbCurves.h) -
            // additive alongside the tank's own synthesised decorrelation,
            // still gated by wetContribution so it is exactly zero
            // whenever the rest of the wet path would be too (wetGain==0
            // or disabled) - a mono input (side==0) leaves this formula
            // identical to before.
            const auto sideCarry = verbInputSideBlend * side;
            L[i] = l + wetContribution * (safeL + sideCarry);
            if (stereo)
                R[i] = r + wetContribution * (safeR - sideCarry);
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
