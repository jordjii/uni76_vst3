#pragma once

#include <algorithm>
#include <cmath>

/*
    Single source of truth for how IMAGE's two independent macro
    parameters - `imager` (0..1, width/imaging amount) and `imageTilt`
    (-1..1 normalised, static L/R balance) - morph the module's
    processing. See Source/DSP/ImagerProcessor and docs/DSP_IMAGE.md.

    IMAGE is a deliberate, explicit exception to the "one knob per
    module" rule (see CLAUDE.md): two independent axes, not one -

        imager (0%..100%):    ORIGINAL -> NATURAL -> WIDE
                               frequency-dependent stereo width: bass
                               gathers toward centre, highs widen. Mid is
                               never touched by this axis - only Side.

        imageTilt (-100..+100): LEFT <- CENTER -> RIGHT
                               a static (non-time-varying) stereo image
                               balance/tilt - NOT a hard pan. Operates on
                               Mid via two independent per-channel shelf
                               gains; Side is never touched by this axis,
                               so existing width is always preserved.

    Both axes evaluate to their identity value at their own zero point
    (imager(0)=width 1.0 at every frequency, imageTilt(0)=gain 1.0 on
    both channels at every frequency) - which is what makes each of the
    three acceptance cases in docs/DSP_IMAGE.md's "Three independent
    identity cases" section (IMAGE=0/TILT=0, IMAGE>0/TILT=0,
    IMAGE=0/TILT!=0) a provable identity, not just a measured one.
*/

namespace uni76::dsp
{
    inline float imagerSmoothstep (float x) noexcept
    {
        const auto c = std::clamp (x, 0.0f, 1.0f);
        return c * c * (3.0f - 2.0f * c);
    }

    inline float imagerLerp (float a, float b, float t) noexcept
    {
        return a + (b - a) * t;
    }

    // ---- IMAGE AMOUNT: frequency-dependent width -----------------------------
    //
    // A single low-shelf filter (not per-channel - this axis is symmetric,
    // it never biases L vs R) reshapes the Side signal only: low
    // frequencies *lose* width as the macro increases (bass "gathers"
    // toward centre - low-end centering/mono-compatibility), high
    // frequencies *gain* width (up to a real mastering-imager-style
    // widen). At t=0 both asymptotes are exactly 1.0, so the shelf
    // collapses to an algebraically exact identity filter (see Biquad.h's
    // makeLowShelf) and Side passes through completely unmodified.
    inline constexpr float imagerWidthMinLow  = 0.25f; // bass narrows toward 25% of its original width at 100%
    inline constexpr float imagerWidthMaxHigh = 2.0f;  // highs widen up to 2x at 100%

    inline float imagerWidthLow (float t01) noexcept
    {
        const auto t = std::clamp (t01, 0.0f, 1.0f);
        return imagerLerp (1.0f, imagerWidthMinLow, imagerSmoothstep (t));
    }

    inline float imagerWidthHigh (float t01) noexcept
    {
        const auto t = std::clamp (t01, 0.0f, 1.0f);
        return imagerLerp (1.0f, imagerWidthMaxHigh, imagerSmoothstep (t));
    }

    // Crossover for the width shelf - shared order of magnitude with PAN's
    // own bass-safety crossover (150Hz, see PanoramaCurves.h) for a
    // consistent "where does this plugin consider content to be bass"
    // convention across modules, not because the two filters are related.
    inline constexpr float imagerWidthCrossoverHz = 150.0f;
    inline constexpr float imagerWidthShelfSlope  = 1.0f; // RBJ "S" - maximally flat, no overshoot (see PanoramaCurves.h's derivation)

    // ---- IMAGE TILT: static L/R balance ---------------------------------------
    //
    // A bounded constant-power gain *pair* applied to Mid only (never
    // Side, which is what keeps existing stereo width fully intact at
    // any tilt setting - see docs/DSP_IMAGE.md's "Stereo Tilt / Balance"
    // section). Framed as an angle theta so gainL^2+gainR^2 == 2 exactly
    // for *any* theta (same algebraic proof PAN's motion rotation uses -
    // see PanoramaProcessor.cpp), which is what keeps combined loudness
    // stable across the whole tilt range rather than just "roughly
    // stable by tuning."
    //
    // thetaMax is deliberately short of the true 45-degree "vanish
    // point" (where cos(45+45)==0, i.e. one channel's Mid contribution
    // would drop to *exactly* zero) - at 42 degrees the quietest channel
    // still keeps a small (~-25dB) but nonzero share of Mid, and its
    // full, untouched Side content throughout, so the "opposite channel
    // must not simply disappear" requirement holds even for a Mid-heavy
    // (low-Side) source, not just for a wide-stereo one.
    inline constexpr float imageTiltThetaMaxDeg = 42.0f;
    inline constexpr float imageTiltThetaMax = imageTiltThetaMaxDeg * 3.14159265358979323846f / 180.0f;

    inline float imageTiltTheta (float tiltNormMinus1to1) noexcept
    {
        const auto t = std::clamp (tiltNormMinus1to1, -1.0f, 1.0f);
        return t * imageTiltThetaMax;
    }

    /** Returns {gainL, gainR} for a given -1..1 tilt value - constant
        power (gainL^2+gainR^2==2 exactly, for any input), symmetric
        (imageTiltGains(-x) == {gainR(x), gainL(x)}), and gainL==gainR==1
        exactly at t=0. This is the *high-frequency asymptote* the tilt
        shelves (below) resolve to - never applied directly as a flat
        gain, see ImagerProcessor.cpp. */
    inline void imageTiltGains (float tiltNormMinus1to1, float& gainL, float& gainR) noexcept
    {
        constexpr float sqrt2 = 1.4142135623730951f;
        constexpr float quarterPi = 0.7853981633974483f;
        const auto theta = imageTiltTheta (tiltNormMinus1to1);
        gainL = sqrt2 * std::cos (quarterPi + theta);
        gainR = sqrt2 * std::sin (quarterPi + theta);
    }

    // ---- IMAGE TILT: frequency-dependent bass safety ---------------------------
    //
    // TILT must not be allowed to drag sub/bass hard into one ear (this
    // would undo IMAGE AMOUNT's own low-end centering philosophy). Rather
    // than a literal 4-breakpoint table (<80Hz/80-150/150-300/300+, as a
    // first product sketch of this control described it), this project's
    // own established, *proven-safe* technique is reused instead: a
    // single per-channel low-shelf filter whose low-frequency asymptote
    // is always exactly 1.0 (no tilt at all at deep bass) and whose
    // high-frequency asymptote is the full tilt gain above - see
    // PanoramaProcessor's width/motion shelves and docs/DSP_PAN.md's
    // "Crossover artifact" section for why a literal band-split-then-sum
    // implementation of a breakpoint table is a proven source of a real
    // frequency-response bump (a vector sum of two differently-gained,
    // phase-shifted bands), and why a single monotonic shelf has no
    // second path to sum against, so no bump is possible by construction.
    // An RBJ "S=1" shelf's own transition is roughly 1-2 octaves wide
    // either side of its corner, which - centred here - already lands
    // close to small-below-80Hz/limited-80-150Hz/appearing-150-300Hz/
    // full-by-300Hz in practice (see docs/DSP_IMAGE.md's measured gain-
    // vs-frequency table), without resorting to the unsafe construction.
    inline constexpr float imageTiltCrossoverHz = 200.0f;
    inline constexpr float imageTiltShelfSlope  = 1.0f;

    // ---- Smoothing -------------------------------------------------------------
    inline constexpr double imagerSmoothingSeconds = 0.03;
}
