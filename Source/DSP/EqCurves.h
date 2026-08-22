#pragma once

#include <algorithm>
#include <cmath>

/*
    Single source of truth for how the EQ / TONE macro parameter (0..1)
    morphs the EQ module's filter network - see Source/DSP/EqProcessor and
    docs/DSP_EQ.md.

    EQ is not `frequency = lerp(min, max, t)`. The knob's centre (t=0.5) is
    a named topology - PHONE, a vintage telephone/radio voice band - not
    just a midpoint between two extremes. The range is two independent
    morph regions sharing that centre point:

        t in [0.0, 0.5]  ->  DARK  ---> PHONE
        t in [0.5, 1.0]  ->  PHONE ---> AIR

    Both regions interpolate the *same* fixed set of five filter stages
    (HP, low shelf, bell/presence, high shelf, LP) between three named
    anchors (DARK/PHONE/AIR) - so there is never a topology switch, only a
    continuous change of each stage's own frequency/gain/Q, and both
    regions agree exactly at t=0.5 (both evaluate to the PHONE anchor),
    which is what makes the crossing genuinely seamless rather than two
    presets stitched together.

    Frequencies interpolate geometrically (log-linear) since that's what
    reads as an even sweep to the ear; gains/Q interpolate linearly. Each
    region's local blend factor is smoothstepped (3t^2-2t^3) rather than
    raw-linear, so the morph's *rate of change* also reaches zero at every
    anchor - not just continuous value, continuous slope too, which is
    what actually guarantees no perceptible "corner" exactly at PHONE.
*/

namespace uni76::dsp
{
    struct EqStageParams
    {
        float hpHz;
        float lowShelfHz, lowShelfDb;
        float bellHz, bellDb, bellQ;
        float highShelfHz, highShelfDb;
        float lpHz;
        float outputTrimDb;
    };

    // ---- Named anchors ---------------------------------------------------
    //
    // DARK (t=0): full bass retained, top gently rounded via a broad LP
    // (not a shelf cut) plus a touch of low-mid warmth. Not an
    // "underwater" effect - LP sits at 5.5kHz, not 1-2kHz.
    inline constexpr EqStageParams eqAnchorDark
    {
        /* hpHz */          28.0f,
        /* lowShelfHz/Db */ 220.0f, 1.6f,
        /* bell Hz/Db/Q */  1000.0f, 0.0f, 0.9f,   // inert at DARK
        /* highShelf Hz/Db */ 4000.0f, 0.0f,        // inert - LP does the rounding
        /* lpHz */          5500.0f,
        /* outputTrimDb */  0.0f,
    };

    // PHONE (t=0.5): a real, clearly audible vintage voice-band character
    // - HP/LP define the band, a broad low-Q bell adds a touch of
    // concentrated presence so the mid stays readable rather than just
    // sounding like "less bass and treble".
    inline constexpr EqStageParams eqAnchorPhone
    {
        /* hpHz */          300.0f,
        /* lowShelfHz/Db */ 220.0f, 0.0f,           // inert - HP defines the low end here
        /* bell Hz/Db/Q */  1500.0f, 2.2f, 0.85f,
        /* highShelf Hz/Db */ 4000.0f, 0.0f,         // inert - LP does the top end
        /* lpHz */          3400.0f,
        /* outputTrimDb */  1.2f,                     // tuned from measurement, see docs/DSP_EQ.md
    };

    // AIR (t=1): bass stays open (gentle HP only), broad high shelf for
    // "air", no big treble spike - not a brittle 10kHz boost.
    inline constexpr EqStageParams eqAnchorAir
    {
        /* hpHz */          70.0f,
        /* lowShelfHz/Db */ 220.0f, 0.0f,            // inert
        /* bell Hz/Db/Q */  4000.0f, 1.2f, 0.7f,      // gentle presence lift
        /* highShelf Hz/Db */ 8500.0f, 4.0f,
        /* lpHz */          20000.0f,                 // effectively open (clamped to Nyquist safely in EqProcessor)
        /* outputTrimDb */  -0.8f,                     // tuned from measurement, see docs/DSP_EQ.md
    };

    inline float eqSmoothstep (float x) noexcept
    {
        const auto c = std::clamp (x, 0.0f, 1.0f);
        return c * c * (3.0f - 2.0f * c);
    }

    inline float eqLerp (float a, float b, float t) noexcept
    {
        return a + (b - a) * t;
    }

    /** Geometric (log-linear) interpolation - reads as an even sweep. */
    inline float eqLerpHz (float a, float b, float t) noexcept
    {
        return std::exp (eqLerp (std::log (a), std::log (b), t));
    }

    inline EqStageParams eqMorph (const EqStageParams& a, const EqStageParams& b, float localT) noexcept
    {
        const auto t = eqSmoothstep (localT);
        return EqStageParams {
            eqLerpHz (a.hpHz, b.hpHz, t),
            eqLerpHz (a.lowShelfHz, b.lowShelfHz, t), eqLerp (a.lowShelfDb, b.lowShelfDb, t),
            eqLerpHz (a.bellHz, b.bellHz, t), eqLerp (a.bellDb, b.bellDb, t), eqLerp (a.bellQ, b.bellQ, t),
            eqLerpHz (a.highShelfHz, b.highShelfHz, t), eqLerp (a.highShelfDb, b.highShelfDb, t),
            eqLerpHz (a.lpHz, b.lpHz, t),
            eqLerp (a.outputTrimDb, b.outputTrimDb, t),
        };
    }

    /** The single entry point: EQ/TONE (0..1) -> the five filter stages'
        parameters, continuous and slope-continuous through t=0.5. */
    inline EqStageParams eqParamsAt (float eqNormalised01) noexcept
    {
        const auto t = std::clamp (eqNormalised01, 0.0f, 1.0f);

        if (t <= 0.5f)
            return eqMorph (eqAnchorDark, eqAnchorPhone, t / 0.5f);

        return eqMorph (eqAnchorPhone, eqAnchorAir, (t - 0.5f) / 0.5f);
    }

    // ---- Smoothing ---------------------------------------------------------
    inline constexpr double eqParameterSmoothingSeconds = 0.035;
    inline constexpr double eqBypassSmoothingSeconds     = 0.02;
}
