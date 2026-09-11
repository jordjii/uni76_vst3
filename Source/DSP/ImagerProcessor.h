#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "Biquad.h"

/*
    UNI 76's IMAGE / STEREO IMAGE module - the seventh and final real DSP
    in the plugin (after PREAMP, EQ, SAT, PITCH, PAN and VERB). See
    CLAUDE.md and docs/DSP_IMAGE.md.

    IMAGE is a deliberate, explicit exception to the plugin's "one knob
    per module" rule: it has TWO independent public parameters -

        imager (-100..+100%): MONO <- CENTER -> STEREO
                               Bipolar frequency-dependent stereo width/
                               imaging (live-testing follow-up round - see
                               ImagerCurves.h's "Bipolar redesign" note):
                               negative values collapse Side toward true
                               mono, positive values widen it (bass
                               gathers toward centre as the macro rises,
                               highs widen - unchanged from the module's
                               original 0..100% contract). Operates on
                               Side only - Mid is never touched by this
                               axis.

        imageTilt (-100..+100): LEFT <- CENTER -> RIGHT
                               A static (time-invariant) stereo image
                               balance/tilt - explicitly NOT a hard L/R
                               pan. Operates on Mid only via a bounded,
                               constant-power, frequency-dependent gain
                               pair - Side is never touched by this axis,
                               so existing stereo width is always fully
                               preserved regardless of tilt.

    Both axes are Mid/Side-domain and orthogonal by construction: imager
    only ever modifies Side, imageTilt only ever modifies Mid. This is
    what makes all three of the documented identity cases (IMAGE=0/
    TILT=0, IMAGE>0/TILT=0, IMAGE=0/TILT!=0) provable rather than merely
    measured - see ImagerCurves.h and docs/DSP_IMAGE.md.

    Both axes reuse PAN's own proven-safe per-channel-shelf technique
    (ImagerCurves.h's ImagerCurves.h comment / docs/DSP_PAN.md's
    "Crossover artifact" section) rather than a band-split-then-sum
    architecture, for the same reason: a single monotonic shelf per
    output path has no second, differently-gained path to vector-sum
    against, so no frequency-response bump is possible by construction.

    Realtime-safety contract matches every other module: prepare() is the
    only place that allocates; process() never allocates, locks, or
    touches the filesystem/WebView.
*/

namespace uni76::dsp
{
    class ImagerProcessor
    {
    public:
        ImagerProcessor() = default;

        void prepare (double sampleRate, int maximumBlockSize, int numChannelsToUse);
        void reset() noexcept;

        /** imageBipolarMinus1to1 (-1..1, `imager`) and
            tiltNormalisedMinus1to1 (-1..1, `imageTilt`) are read once per
            call and smoothed internally - passing raw (possibly jumpy)
            automation values each block is safe and expected. Both are
            static/time-invariant by design (no internal LFO, unlike PAN)
            - see the class comment. Mono buses (numChannels < 2) are left
            completely untouched - there is no L/R balance or width to
            build in a true mono bus (see docs/DSP_IMAGE.md's "Real mono
            bus" section). */
        void process (juce::AudioBuffer<float>& buffer, float imageBipolarMinus1to1, float tiltNormalisedMinus1to1, bool enabled) noexcept;

        /** Always 0 - IMAGE is a pure gain/filter morph with no
            oversampling, no lookahead, no delay-based widening (no
            Haas), no time-varying modulation of any kind. */
        int getLatencySamples() const noexcept { return 0; }

    private:
        double sampleRate = 44100.0;
        int numChannels = 2;

        // IMAGE AMOUNT's single width shelf, applied to Side only - see
        // the class comment and ImagerCurves.h's "IMAGE AMOUNT" section.
        Biquad widthShelf;

        // IMAGE TILT's two independent per-channel shelves, applied to
        // Mid only - see the class comment and ImagerCurves.h's "IMAGE
        // TILT" section.
        Biquad tiltShelfL, tiltShelfR;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> imageSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> tiltSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassSmoother;

        // No latency to align against (getLatencySamples() == 0), so the
        // dry copy needs no delay line - same EQ/PAN-style bypass pattern
        // (immediate crossfade, no IntegerDelayLine).
        juce::AudioBuffer<float> dryScratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ImagerProcessor)
    };
}
