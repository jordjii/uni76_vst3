#pragma once

#include <algorithm>

/*
    Single source of truth for how the PAN / STEREO FIELD macro parameter
    (0..1, the `panorama` APVTS value) morphs the module's width and
    motion behaviour - see Source/DSP/PanoramaProcessor and
    docs/DSP_PAN.md.

    Despite the parameter's internal ID, this is NOT an L/R balance pan -
    it is a combined stereo *width + motion* control:

        0.0  = ORIGINAL  (bit-exact identity - see below)
        0.5  = WIDE       (moderate static width, moderate slow motion)
        1.0  = MOTION      (wide, with obvious slow ear-to-ear movement)

    Unlike EqCurves.h's DARK-PHONE-AIR or the previous MONO-NATURAL-WIDE
    revision of this same file, there is no named anchor at 0.5 here -
    every curve below is a single smoothstepped sweep from t=0 to t=1.
    The one hard requirement these curves must satisfy exactly, not just
    approximately, is that **every one of them evaluates to its identity
    value at t=0** (width=1.0, motion depth=0.0, induced-decorrelation
    blend=0.0) - that is what makes ORIGINAL a true, provable bypass
    rather than a measured approximation (see PanoramaProcessor.cpp's
    process(), and docs/DSP_PAN.md's "ORIGINAL = 0%" section).
*/

namespace uni76::dsp
{
    inline float panSmoothstep (float x) noexcept
    {
        const auto c = std::clamp (x, 0.0f, 1.0f);
        return c * c * (3.0f - 2.0f * c);
    }

    inline float panLerp (float a, float b, float t) noexcept
    {
        return a + (b - a) * t;
    }

    // ---- Static width ---------------------------------------------------
    //
    // width(0) = 1.0 exactly (identity gain, not a mono-collapsing 0.0 the
    // way the old MONO/NATURAL/WIDE curve used) - width now only ever
    // *adds* to the existing stereo image, never subtracts from it. Two
    // independently-tunable ceilings (high/low band, see the crossover
    // below) both share this same t=0 starting point.
    inline constexpr float panWidthMaxHigh = 1.9f; // high band ceiling at t=1.0 (WIDE/MOTION range: ~1.8-2.0)
    inline constexpr float panWidthMaxLow  = 1.15f; // low band ceiling at t=1.0 - bass barely widens

    inline float panWidthGain (float t01, float maxAtFull) noexcept
    {
        const auto t = std::clamp (t01, 0.0f, 1.0f);
        return panLerp (1.0f, maxAtFull, panSmoothstep (t));
    }

    // ---- Motion depth -----------------------------------------------------
    //
    // motionDepth(0) = 0.0 exactly - no LFO influence whatsoever at
    // ORIGINAL, regardless of the LFO's own free-running phase (see
    // PanoramaProcessor - the LFO clock never stops or resets, only its
    // *effect* is gated to zero here). 1.0 at motionDepthMaxHigh represents
    // a strong, obvious swing (see panMotionThetaRange below for how depth
    // maps to an actual left/right energy bias); the low band's ceiling is
    // deliberately far smaller so bass motion stays close to inaudible.
    inline constexpr float panMotionDepthMaxHigh = 0.85f;
    inline constexpr float panMotionDepthMaxLow  = 0.12f;

    inline float panMotionDepth (float t01, float maxAtFull) noexcept
    {
        const auto t = std::clamp (t01, 0.0f, 1.0f);
        return maxAtFull * panSmoothstep (t);
    }

    // ---- Induced (mono-compatible) decorrelation blend ---------------------
    //
    // How much of the phase-decorrelated "induced" signal (derived from
    // Mid via an allpass - see PanoramaProcessor) is blended into the
    // spatial signal alongside the real Side content. 0.0 at t=0 (so a
    // stereo source's *real* Side content is the only thing ORIGINAL ever
    // reproduces - never synthesised content); grows with t so that a
    // genuinely mono source (Side==0 identically) still gains a real,
    // phase-safe spatial/motion field as the knob turns up.
    inline constexpr float panInducedBlendMax = 0.55f;

    inline float panInducedBlend (float t01) noexcept
    {
        const auto t = std::clamp (t01, 0.0f, 1.0f);
        return panInducedBlendMax * panSmoothstep (t);
    }

    // ---- Motion LFO ---------------------------------------------------------
    //
    // A single, deterministic, phase-continuous clock shared by both
    // bands (so the whole spatial field moves together, not independently
    // per band) - free-running from the moment prepare() is called,
    // *never* reset or re-phased by a parameter change (only reset()
    // - i.e. playback stop/restart - resets it, same as every other
    // module's filter state). ~0.3Hz -> a full Left-Center-Right-Center-
    // Left cycle takes a bit over 3 seconds - deliberately slow, nothing
    // resembling tremolo.
    inline constexpr double panLfoRateHz = 0.3;

    // Equal-power motion: at depth=0, theta sits at the fixed centre
    // pi/4 (cos==sin==1/sqrt(2), i.e. the ordinary symmetric-width
    // gainL==gainR==1 case). As depth grows, theta swings further away
    // from that centre, following the LFO - see
    // PanoramaProcessor::process() for the constant-power proof
    // (gainL^2 + gainR^2 == 2 for *any* theta, algebraically, not just
    // measured) this angle-based formulation provides.
    inline constexpr float panMotionThetaCentre = 0.7853981633974483f; // pi/4
    inline constexpr float panMotionThetaRange  = 0.7853981633974483f; // pi/4 - depth=1 swings a full quarter-turn either way

    // ---- Low/high band split -----------------------------------------------
    //
    // A single first-order (6dB/oct) one-pole lowpass splits the spatial
    // signal into spatialLow/spatialHigh - deliberately gentler than the
    // previous (MONO/NATURAL/WIDE) revision's 2nd-order Butterworth.
    // That revision measured a ~5.5% Side-gain overshoot right at its
    // crossover whenever the two bands carried different gains (a
    // consequence of the 2nd-order filter's steeper phase excursion
    // interacting with the complementary-band vector sum - see
    // docs/DSP_PAN.md's "Frequency-dependent motion and width" section
    // for the full explanation and the measured before/after). A 1st-order
    // split has a much smaller worst-case phase shift (max 90 degrees
    // instead of 180), which measurably reduces that overshoot, and is
    // *simpler* than what it replaces, not more complex. spatialHigh is
    // still defined as the exact complement (spatial - LP(spatial)), so
    // spatialLow + spatialHigh == spatial identically for any filter
    // shape - the same identity-preserving property the previous design
    // relied on for its NATURAL point is what makes ORIGINAL (t=0, both
    // bands' width gain 1.0 and motion depth 0.0) exact here too.
    inline constexpr float panCrossoverHz = 150.0f;

    // ---- Induced-decorrelation allpass --------------------------------------
    //
    // A single 2nd-order allpass (unity magnitude at every frequency,
    // always - see Biquad.h's makeAllpass), centred in the "main motion"
    // band (roughly 250Hz-2kHz per the product brief) - not a delay, so
    // it cannot introduce comb filtering, wow/flutter, or pitch drift.
    //
    // A 2nd-order allpass's phase is 0 at DC, -180 degrees at its own
    // centre frequency, and -360 degrees approaching Nyquist - so
    // *quadrature* (90 degrees, i.e. time-average-uncorrelated with Mid:
    // E[Mid*allpass(Mid)] = 0.5*A^2*cos(phaseShift), zero only at +/-90
    // degrees) happens at two frequencies flanking the centre, not at the
    // centre itself. Away from those two points, the correlation is
    // measurably nonzero and *changes sign* either side of them - for a
    // single sustained tone at an unlucky frequency, that shows up as a
    // real, audible problem: the reconstructed spatial field ends up
    // biased toward one side almost all the time instead of swinging
    // left-right (caught and measured during this module's development -
    // see docs/DSP_PAN.md's "Mono-to-stereo strategy" section for the
    // measured before/after, including a rejected 2-stage-cascade attempt
    // that changed *which* frequencies were biased without reducing the
    // problem generally). No single fixed filter can sit at quadrature
    // for every possible source frequency simultaneously - what actually
    // matters is realistic (broadband/harmonic, not single-sine) mono
    // material, where energy at many different frequencies contributes
    // correlations of different signs that substantially cancel in
    // aggregate; the module's own test suite verifies motion using such
    // material rather than a single sustained tone for exactly this
    // reason.
    inline constexpr float panAllpassHz = 900.0f;
    inline constexpr float panAllpassQ  = 0.6f;

    // ---- Smoothing -----------------------------------------------------------
    inline constexpr double panSmoothingSeconds = 0.02;
}
