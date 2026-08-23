#include "PanoramaProcessor.h"
#include "PanoramaCurves.h"

#include <algorithm>
#include <cmath>

namespace uni76::dsp
{
    void PanoramaProcessor::prepare (double sampleRateIn, int maximumBlockSize, int numChannelsToUse)
    {
        sampleRate = sampleRateIn;
        numChannels = juce::jlimit (1, 2, numChannelsToUse);

        makeAllpass (midAllpass, sampleRate, panAllpassHz, panAllpassQ);
        inducedLowpass.setCutoffHz (sampleRate, panCrossoverHz);

        dryScratch.setSize (2, maximumBlockSize, false, false, true);

        widthSmoother.reset (sampleRate, panSmoothingSeconds);
        bypassSmoother.reset (sampleRate, panSmoothingSeconds);
        // ORIGINAL (0%) is the new product default and the module's own
        // identity point - matches ParameterLayout.cpp's default so a
        // freshly prepared instance never ramps from a wrong value before
        // the host's first real parameter update arrives.
        widthSmoother.setCurrentAndTargetValue (0.0f);
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        reset();
    }

    void PanoramaProcessor::reset() noexcept
    {
        midAllpass.reset();
        inducedLowpass.reset();
        shelfL.reset();
        shelfR.reset();
        lfoPhase = 0.0;
    }

    void PanoramaProcessor::process (juce::AudioBuffer<float>& buffer, float widthNormalised01, bool enabled) noexcept
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

        widthSmoother.setTargetValue (std::clamp (widthNormalised01, 0.0f, 1.0f));
        bypassSmoother.setTargetValue (enabled ? 1.0f : 0.0f);

        // Mono buses have no stereo field to build - a true no-op (not
        // "fabricate stereo from nothing"). The sanitised input above is
        // already the correct output. The LFO clock still advances below
        // this early-return only when channels>=2 - a mono bus's clock
        // simply doesn't run, which is fine: there is nothing it could
        // ever be heard to affect.
        if (channels < 2 || numChannels < 2)
            return;

        // ---- dry copy for the enable/disable crossfade (no latency to
        // align against, unlike PREAMP/SAT/PITCH's IntegerDelayLine) ----
        dryScratch.copyFrom (0, 0, buffer, 0, 0, numSamples);
        dryScratch.copyFrom (1, 0, buffer, 1, 0, numSamples);

        auto* L = buffer.getWritePointer (0);
        auto* R = buffer.getWritePointer (1);
        const auto* dryL = dryScratch.getReadPointer (0);
        const auto* dryR = dryScratch.getReadPointer (1);

        const auto lfoIncrement = twoPi * panLfoRateHz / sampleRate;
        constexpr float sqrt2 = 1.4142135623730951f;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto t = widthSmoother.getNextValue();
            const auto mix = bypassSmoother.getNextValue();

            const auto l = L[i];
            const auto r = R[i];

            // Mid/Side matrix (0.5-scaled convention - see docs/DSP_PAN.md).
            const auto mid  = 0.5f * (l + r);
            const auto side = 0.5f * (l - r);

            // Phase-decorrelated reference for Mid - the allpass output
            // itself (not a difference against Mid - see PanoramaCurves.h
            // for why: `allpass(Mid) - Mid` is *always* negatively
            // correlated with Mid for any nonzero phase shift, which
            // reliably biases the reconstructed L/R energies toward one
            // side even at motion depth 0, which the raw allpass output
            // is not). No single fixed allpass is at quadrature (zero
            // correlation with Mid) for every possible source frequency -
            // see PanoramaCurves.h's class comment for the full
            // reasoning and why realistic (broadband) material is what
            // actually matters here, not a single sustained tone.
            const auto induced = midAllpass.processSample (mid);

            // `induced` is derived from the *full* Mid signal, which
            // includes any real bass the source has - without removing
            // that bass-frequency content here, synthesised spatial
            // energy would leak into the low band and get width/motion-
            // processed there too (even though the low-band ceilings are
            // small), moving bass that was never really stereo to begin
            // with. `inducedHigh` is what's left after subtracting the
            // induced signal's own low band.
            const auto inducedLow  = inducedLowpass.processSample (induced);
            const auto inducedHigh = induced - inducedLow;

            // t=0 (ORIGINAL) forces panInducedBlend(0)==0.0 exactly, so
            // spatialRaw == side exactly here regardless of `induced`'s
            // value - real Side content is the only thing ORIGINAL ever
            // reproduces, never synthesised content.
            const auto spatialRaw = side + inducedHigh * panInducedBlend (t);

            const auto widthLow  = panWidthGain (t, panWidthMaxLow);
            const auto widthHigh = panWidthGain (t, panWidthMaxHigh);
            const auto motionLow  = panMotionDepth (t, panMotionDepthMaxLow);
            const auto motionHigh = panMotionDepth (t, panMotionDepthMaxHigh);

            const auto lfoSin = std::sin ((float) lfoPhase);

            // t=0 forces motionLow==motionHigh==0.0 exactly, so both
            // thetas collapse to the fixed centre regardless of lfoSin -
            // the LFO's own phase never affects ORIGINAL's output.
            const auto thetaLow  = panMotionThetaCentre + motionLow  * lfoSin * panMotionThetaRange;
            const auto thetaHigh = panMotionThetaCentre + motionHigh * lfoSin * panMotionThetaRange;

            // Equal-power rotation baked directly into each band's gain:
            // gainLow^2 + gainHigh^2 (same channel) == width^2 * ((sqrt2
            // *cos)^2+(sqrt2*sin)^2) == 2*width^2 at that band's own
            // asymptote frequency, for *any* theta - an algebraic
            // identity at each shelf asymptote, not a measured
            // approximation. At theta==thetaCentre (pi/4) and width==1,
            // gainL==gainR==1.0, i.e. plain symmetric width with no
            // motion bias.
            const auto gainLLow  = widthLow  * sqrt2 * std::cos (thetaLow);
            const auto gainLHigh = widthHigh * sqrt2 * std::cos (thetaHigh);
            const auto gainRLow  = widthLow  * sqrt2 * std::sin (thetaLow);
            const auto gainRHigh = widthHigh * sqrt2 * std::sin (thetaHigh);

            // Each output channel gets its own single monotonic low-shelf
            // (not a crossover split shared between channels) whose low/
            // high asymptotes are exactly that channel's own low/high-
            // band gain above - see the class comment in
            // PanoramaProcessor.h and docs/DSP_PAN.md's "Crossover
            // artifact" section for why this construction cannot produce
            // a vector-sum bump the way splitting spatialRaw into two
            // *shared* bands and weighting each differently did. At t=0,
            // gainLLow==gainLHigh==gainRLow==gainRHigh==1.0 exactly (see
            // above), so both shelves' gainDb collapses to exactly 0dB -
            // an algebraically exact identity filter (see Biquad.h's
            // makeLowShelf) - and toL==toR==spatialRaw==side exactly.
            constexpr float minGain = 1.0e-6f;
            const auto gainDbL = 20.0f * std::log10 (std::max (minGain, gainLLow) / std::max (minGain, gainLHigh));
            makeLowShelf (shelfL, sampleRate, panCrossoverHz, gainDbL, panShelfSlope);
            const auto toL = shelfL.processSample (spatialRaw) * gainLHigh;

            const auto gainDbR = 20.0f * std::log10 (std::max (minGain, gainRLow) / std::max (minGain, gainRHigh));
            makeLowShelf (shelfR, sampleRate, panCrossoverHz, gainDbR, panShelfSlope);
            const auto toR = shelfR.processSample (spatialRaw) * gainRHigh;

            // Mid is never touched - at t=0, toL==toR==spatialRaw==side
            // exactly, so wetL/wetR collapse to the exact input L/R - see
            // the class comment in PanoramaProcessor.h.
            const auto wetL = mid + toL;
            const auto wetR = mid - toR;

            L[i] = dryL[i] * (1.0f - mix) + wetL * mix;
            R[i] = dryR[i] * (1.0f - mix) + wetR * mix;

            lfoPhase += lfoIncrement;
            if (lfoPhase >= twoPi)
                lfoPhase -= twoPi;
        }

        // ---- final numeric safety net ------------------------------------
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                if (! std::isfinite (data[i]))
                    data[i] = 0.0f;
        }
    }
}
