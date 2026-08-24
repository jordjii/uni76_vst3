#include "ImagerProcessor.h"
#include "ImagerCurves.h"

#include <algorithm>
#include <cmath>

namespace uni76::dsp
{
    void ImagerProcessor::prepare (double sampleRateIn, int maximumBlockSize, int numChannelsToUse)
    {
        sampleRate = sampleRateIn;
        numChannels = juce::jlimit (1, 2, numChannelsToUse);

        dryScratch.setSize (2, maximumBlockSize, false, false, true);

        imageSmoother.reset (sampleRate, imagerSmoothingSeconds);
        tiltSmoother.reset (sampleRate, imagerSmoothingSeconds);
        bypassSmoother.reset (sampleRate, imagerSmoothingSeconds);
        // ORIGINAL/CENTER (imager=0, imageTilt=0) is this module's own
        // identity point - matches ParameterLayout.cpp's defaults so a
        // freshly prepared instance never ramps from a wrong value
        // before the host's first real parameter update arrives.
        imageSmoother.setCurrentAndTargetValue (0.0f);
        tiltSmoother.setCurrentAndTargetValue (0.0f);
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        reset();
    }

    void ImagerProcessor::reset() noexcept
    {
        widthShelf.reset();
        tiltShelfL.reset();
        tiltShelfR.reset();
    }

    void ImagerProcessor::process (juce::AudioBuffer<float>& buffer, float imageNormalised01, float tiltNormalisedMinus1to1, bool enabled) noexcept
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

        // std::clamp does not clamp NaN (see docs/DSP_VERB.md's "A real
        // bug found by Debug-mode testing" section) - guard both macro
        // parameters before they ever reach a smoother, matching the
        // established defence-in-depth pattern for every macro parameter
        // in this plugin.
        const auto safeImage = std::isfinite (imageNormalised01) ? imageNormalised01 : 0.0f;
        const auto safeTilt  = std::isfinite (tiltNormalisedMinus1to1) ? tiltNormalisedMinus1to1 : 0.0f;

        imageSmoother.setTargetValue (std::clamp (safeImage, 0.0f, 1.0f));
        tiltSmoother.setTargetValue (std::clamp (safeTilt, -1.0f, 1.0f));
        bypassSmoother.setTargetValue (enabled ? 1.0f : 0.0f);

        // A real mono bus has no L/R balance or width to build - a true
        // no-op (not "fabricate stereo from nothing"), matching PAN's
        // own early-return for mono buses. The sanitised input above is
        // already the correct output.
        if (channels < 2 || numChannels < 2)
        {
            imageSmoother.skip (numSamples);
            tiltSmoother.skip (numSamples);
            bypassSmoother.skip (numSamples);
            return;
        }

        // ---- dry copy for the enable/disable crossfade ----------------
        dryScratch.copyFrom (0, 0, buffer, 0, 0, numSamples);
        dryScratch.copyFrom (1, 0, buffer, 1, 0, numSamples);

        auto* L = buffer.getWritePointer (0);
        auto* R = buffer.getWritePointer (1);
        const auto* dryL = dryScratch.getReadPointer (0);
        const auto* dryR = dryScratch.getReadPointer (1);

        // Coefficients derived from the macro values are recomputed once
        // per block (from the smoothed value settled at the *start* of
        // this block), not per sample - same one-block-lag pattern EQ/
        // SAT/VERB already use. Unlike PAN, neither imager nor imageTilt
        // has any audio-rate modulation of its own (imageTilt is
        // explicitly static - see the class comment), so there is no
        // audible-staircase risk the way PAN's LFO-driven rotation had.
        const auto t = imageSmoother.getCurrentValue();
        const auto tiltT = tiltSmoother.getCurrentValue();

        constexpr float minGain = 1.0e-6f;

        const auto widthLow = imagerWidthLow (t);
        const auto widthHigh = imagerWidthHigh (t);
        // At t=0, widthLow==widthHigh==1.0 exactly, so gainDb==0dB
        // exactly - widthShelf collapses to an algebraically exact
        // identity filter (see Biquad.h's makeLowShelf), and Side passes
        // through completely unmodified.
        const auto widthGainDb = 20.0f * std::log10 (std::max (minGain, widthLow) / std::max (minGain, widthHigh));
        makeLowShelf (widthShelf, sampleRate, imagerWidthCrossoverHz, widthGainDb, imagerWidthShelfSlope);

        float tiltGainL = 1.0f, tiltGainR = 1.0f;
        imageTiltGains (tiltT, tiltGainL, tiltGainR);
        // Low asymptote is always exactly 1.0 (no tilt at deep bass);
        // high asymptote is the full tilt gain for that channel - see
        // ImagerCurves.h's "IMAGE TILT: frequency-dependent bass safety"
        // section. At tiltT=0, tiltGainL==tiltGainR==1.0 exactly, so
        // both gainDb values are 0dB exactly and both shelves collapse
        // to an exact identity filter.
        const auto tiltGainDbL = 20.0f * std::log10 (1.0f / std::max (minGain, tiltGainL));
        const auto tiltGainDbR = 20.0f * std::log10 (1.0f / std::max (minGain, tiltGainR));
        makeLowShelf (tiltShelfL, sampleRate, imageTiltCrossoverHz, tiltGainDbL, imageTiltShelfSlope);
        makeLowShelf (tiltShelfR, sampleRate, imageTiltCrossoverHz, tiltGainDbR, imageTiltShelfSlope);

        imageSmoother.skip (numSamples);
        tiltSmoother.skip (numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto mix = bypassSmoother.getNextValue();

            const auto l = L[i];
            const auto r = R[i];

            // Mid/Side matrix (0.5-scaled convention - matches PAN/VERB).
            const auto mid  = 0.5f * (l + r);
            const auto side = 0.5f * (l - r);

            // IMAGE AMOUNT: Side only, symmetric between channels - never
            // biases L vs R. At imager=0 this is side unchanged exactly.
            const auto sideProcessed = widthShelf.processSample (side) * widthHigh;

            // IMAGE TILT: Mid only, via two independent per-channel
            // shelves - never touches Side, so existing width is always
            // fully preserved regardless of tilt. At imageTilt=0 both
            // filtered values equal mid exactly.
            const auto filteredMidL = tiltShelfL.processSample (mid) * tiltGainL;
            const auto filteredMidR = tiltShelfR.processSample (mid) * tiltGainR;

            const auto wetL = filteredMidL + sideProcessed;
            const auto wetR = filteredMidR - sideProcessed;

            L[i] = dryL[i] * (1.0f - mix) + wetL * mix;
            R[i] = dryR[i] * (1.0f - mix) + wetR * mix;
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
