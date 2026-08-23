#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "Biquad.h"

/*
    UNI 76's PAN / STEREO FIELD module - the fifth real DSP in the plugin
    (after PREAMP, EQ, SAT and PITCH). VERB/IMAGE stay passthrough - see
    CLAUDE.md and docs/DSP_PAN.md.

    NOT an L/R balance pan, despite the `panorama` parameter ID (kept only
    for compatibility - never renamed). It is a combined stereo width +
    slow ear-to-ear motion control:

        0%   = ORIGINAL  - bit-exact (up to float rounding) identity.
        50%  = WIDE       - moderate static width, moderate slow motion.
        100% = MOTION      - wide, with an obvious slow L<->R swing.

    Architecture: Mid (0.5*(L+R)) is always the stable "core" - every
    watt of width/motion comes from a separately-processed "spatial"
    signal (built from the real Side content plus, for mono/near-mono
    sources, a phase-decorrelated "induced" component derived from Mid via
    an allpass - no delay, so no comb filtering, no wow/flutter, no pitch
    drift), reshaped into L and R contributions by two independent
    low-shelf filters (one per output channel) rather than a crossover
    split - see "Crossover artifact" in docs/DSP_PAN.md for why: splitting
    the spatial signal into low/high *bands* and then multiplying each
    band by a *different real gain* before summing is a textbook source of
    a frequency-response bump right at the crossover (the two bands are
    phase-shifted relative to each other, so unequal-gain summing is a
    vector sum, not a linear interpolation). A single monotonic shelf per
    channel has no second, differently-gained path to sum against, so no
    bump is possible by construction, not just by tuning. Both shelves'
    low/high asymptote gains already encode that channel's own width AND
    motion-rotation ceiling for that frequency region, driven by a slow
    (~0.3Hz), free-running, deterministic LFO - a stable centre with the
    *surrounding* space moving, not a global auto-pan of the whole signal.

    Every curve in PanoramaCurves.h evaluates to its identity value at
    t=0 (width gain 1.0, motion depth 0.0, induced blend 0.0) - which is
    what makes ORIGINAL provably (not just measured) a bypass: with those
    values, the spatial-signal reconstruction algebraically collapses
    back to L = Mid+Side, R = Mid-Side, i.e. the unmodified input.

    Realtime-safety contract matches every other module: prepare() is the
    only place that allocates; process() never allocates, locks, or
    touches the filesystem/WebView.
*/

namespace uni76::dsp
{
    class PanoramaProcessor
    {
    public:
        PanoramaProcessor() = default;

        void prepare (double sampleRate, int maximumBlockSize, int numChannelsToUse);
        void reset() noexcept;

        /** widthNormalised01 and enabled are read once per call - both are
            smoothed internally (~20ms), so passing a raw (possibly jumpy)
            automation value each block is safe and expected. The motion
            LFO's own phase is a free-running clock, entirely independent
            of widthNormalised01 changes - only reset() (not a parameter
            change) resets it. Mono buses (numChannels < 2) are left
            untouched - there is no stereo field to build. */
        void process (juce::AudioBuffer<float>& buffer, float widthNormalised01, bool enabled) noexcept;

        /** Always 0 - PAN is a pure gain/filter/rotation morph with no
            oversampling, no lookahead, no delay-based motion (no Haas).
            Constant for the lifetime of a prepare() call, like every
            other module's getLatencySamples(). */
        int getLatencySamples() const noexcept { return 0; }

    private:
        double sampleRate = 44100.0;
        int numChannels = 2;

        // Derives the phase-decorrelated "induced" signal from Mid (mono/
        // near-mono sources' only source of spatial content) - unity
        // magnitude at every frequency, always, so it adds no coloration
        // and no delay-based artifact - see PanoramaCurves.h for why a
        // single fixed allpass can't be perfectly uncorrelated with Mid
        // for every possible source frequency, and why that's fine for
        // realistic (non-single-tone) material.
        Biquad midAllpass;

        // Removes `induced`'s own bass-frequency content before it is
        // blended into the spatial signal - Mid (what the allpass reads)
        // contains the source's real bass, and without this, synthesised
        // "induced" energy would leak into the low band and get width/
        // motion-processed there too, even though the low-band ceilings
        // are small - moving bass that was never really stereo to begin
        // with. Real Side content is not filtered this way - only ever
        // the synthesised component is excluded from the bass region.
        // Deliberately a gentle 1-pole (not steeper): a 2nd-order
        // Butterworth was tried here during this round's crossover
        // rework (reasoning that a steeper cutoff should leak less of
        // induced's own bass) and measured *worse* (centre-core L/R
        // imbalance rose from ~2.6dB to ~3.4dB) - a 2nd-order filter's
        // larger phase excursion (up to 180 degrees vs a 1-pole's 90)
        // reintroduces the same complementary-subtraction phase-mismatch
        // mechanism the width/motion shelves were rebuilt to eliminate,
        // just applied to `induced` instead of to spatialRaw. Reverted;
        // see docs/DSP_PAN.md's "Crossover artifact" section.
        OnePoleLowPass inducedLowpass;

        // The two channel-output shelves - see the class comment above and
        // docs/DSP_PAN.md's "Crossover artifact" section. Coefficients are
        // recomputed every sample (not once per block, unlike EqProcessor):
        // their target gain is itself audio-rate (driven by the free-
        // running motion LFO), not just a slow user/automation macro, so a
        // per-block update would show up as an audible staircase in the
        // motion trajectory at large host block sizes.
        Biquad shelfL, shelfR;

        // Free-running motion clock, radians, wrapped to [0, 2*pi) each
        // sample - never reset by a parameter change, only by reset().
        double lfoPhase = 0.0;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassSmoother;

        // No latency to align against (getLatencySamples() == 0), so the
        // dry copy needs no delay line - same EQ-style bypass pattern
        // (immediate crossfade, no IntegerDelayLine).
        juce::AudioBuffer<float> dryScratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanoramaProcessor)
    };
}
