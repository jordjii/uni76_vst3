#include "PanoramaProcessor.h"
#include "PanoramaCurves.h"

#include <cmath>

namespace uni76::dsp
{
    void PanoramaProcessor::prepare (double sampleRateIn, int maximumBlockSize, int numChannelsToUse)
    {
        sampleRate = sampleRateIn;
        numChannels = juce::jlimit (1, 2, numChannelsToUse);

        makeLowPassButterworth (sideLowpass, sampleRate, panCrossoverHz);

        dryScratch.setSize (2, maximumBlockSize, false, false, true);

        widthSmoother.reset (sampleRate, panSmoothingSeconds);
        bypassSmoother.reset (sampleRate, panSmoothingSeconds);
        // NATURAL (identity) is the correct resting value - matches the
        // new product default (50%, see ParameterLayout.cpp) and means a
        // freshly prepared instance never ramps from a wrong starting
        // point before the host's first real parameter update arrives.
        widthSmoother.setCurrentAndTargetValue (0.5f);
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        reset();
    }

    void PanoramaProcessor::reset() noexcept
    {
        sideLowpass.reset();
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

        // Mono buses have no stereo field to widen - a true no-op (not
        // "fabricate stereo from nothing"). The sanitised input above is
        // already the correct output.
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

        for (int i = 0; i < numSamples; ++i)
        {
            const auto t = widthSmoother.getNextValue();
            const auto mix = bypassSmoother.getNextValue();

            const auto l = L[i];
            const auto r = R[i];

            // Mid/Side matrix (0.5-scaled convention: M=R=L when L==R
            // gives mid==L, side==0, and the L=M+S / R=M-S reconstruction
            // below is then exactly the identity - no separate decode
            // gain needed).
            const auto mid  = 0.5f * (l + r);
            const auto side = 0.5f * (l - r);

            const auto sideLow  = sideLowpass.processSample (side);
            const auto sideHigh = side - sideLow; // exact complement - see PanoramaCurves.h

            const auto lowGain  = panWidthGain (t, panWidthMaxLow);
            const auto highGain = panWidthGain (t, panWidthMaxHigh);

            const auto sideOut = lowGain * sideLow + highGain * sideHigh;

            // Mid is never touched - mono fold-down ((wetL+wetR)/2 == mid)
            // is therefore bit-identical to the input's own mono sum at
            // every width setting, by construction, not by measurement.
            const auto wetL = mid + sideOut;
            const auto wetR = mid - sideOut;

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
