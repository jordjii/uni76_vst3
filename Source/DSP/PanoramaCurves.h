#pragma once

#include <algorithm>

/*
    Single source of truth for how the PAN / STEREO FIELD macro parameter
    (0..1, the `panorama` APVTS value) morphs the module's Mid/Side width
    gain - see Source/DSP/PanoramaProcessor and docs/DSP_PAN.md.

    Despite the parameter's internal ID, this is NOT an L/R balance pan -
    it is a stereo *width* control:

        0.0  = MONO     (Side gain -> 0, output collapses to a correct
                          mono sum)
        0.5  = NATURAL  (Side gain = 1.0, mathematically the identity
                          transform - the original stereo image, unchanged)
        1.0  = WIDE     (Side gain > 1.0, audibly wider)

    Two linear-in-smoothstep segments sharing 0.5 as a named centre -
    exactly the same shape EqCurves.h uses for DARK-PHONE-AIR, for the
    same reason: a knob whose *centre* is the reference point (not one end
    of a sweep) reads better as two regions meeting at a real anchor than
    as one continuous curve that merely happens to pass through 1.0
    partway along. Using smoothstep (not raw-linear) makes the curve's
    *slope* reach zero at 0%/50%/100% too, not just its value - so
    turning the knob near MONO, NATURAL or WIDE feels less twitchy than
    right in the middle of a region, and there is no perceptible "corner"
    exactly at NATURAL.

    A second, independently-tunable ceiling (panWidthMaxLow) exists
    because width is frequency-dependent - see "Low-end protection" in
    docs/DSP_PAN.md. Both ceilings apply only to the 0.5->1.0 half; the
    0.0->0.5 half is shared and identical regardless of ceiling, which is
    what guarantees MONO collapses *every* frequency equally (0.0 always
    means Side gain 0.0, full stop) and NATURAL is the identity transform
    at every frequency (0.5 always means Side gain 1.0, full stop) - only
    how far WIDE pushes past 1.0 differs between the low and high band.
*/

namespace uni76::dsp
{
    // ---- Width gain ceilings at t=1.0 (WIDE) --------------------------
    //
    // High band (above the crossover - see panCrossoverHz below): pushed
    // to an audibly wide, still-usable 1.8x Side gain - within the
    // product brief's ~1.7-2.0 target range for 100%.
    inline constexpr float panWidthMaxHigh = 1.8f;

    // Low band (below the crossover): deliberately a much smaller
    // ceiling - bass stays close to its original width even at full
    // WIDE, rather than getting proportionally as wide as the highs
    // (which is exactly the phase-cancellation/mono-compatibility risk
    // the product brief calls out for sub/kick material).
    inline constexpr float panWidthMaxLow = 1.15f;

    inline float panSmoothstep (float x) noexcept
    {
        const auto c = std::clamp (x, 0.0f, 1.0f);
        return c * c * (3.0f - 2.0f * c);
    }

    inline float panLerp (float a, float b, float t) noexcept
    {
        return a + (b - a) * t;
    }

    /** widthNormalised01 (the raw `panorama` APVTS value, 0..1) -> a Side
        channel gain multiplier, for either the low-band or high-band
        ceiling. Both ceilings evaluate identically for t<=0.5 by
        construction (see the class comment above) - only maxAtFull
        (evaluated only for t>0.5) differs between bands. */
    inline float panWidthGain (float widthNormalised01, float maxAtFull) noexcept
    {
        const auto t = std::clamp (widthNormalised01, 0.0f, 1.0f);

        if (t <= 0.5f)
            return panLerp (0.0f, 1.0f, panSmoothstep (t / 0.5f));

        return panLerp (1.0f, maxAtFull, panSmoothstep ((t - 0.5f) / 0.5f));
    }

    // ---- Low/high band split ------------------------------------------
    //
    // A single 2nd-order (12dB/oct) Butterworth lowpass splits the Side
    // signal into sideLow = LP(side) and sideHigh = side - sideLow. Not a
    // brickwall crossover - a gentle single-pole-pair rolloff centred in
    // the product brief's 120-300Hz "smooth transition" zone, gliding
    // from "close to the low-band ceiling" around 60-100Hz to "close to
    // the high-band ceiling" by ~300Hz+. Because sideHigh is defined as
    // the *complement* of sideLow (side - LP(side), not a second,
    // independently-designed filter), sideLow + sideHigh == side exactly
    // for any filter shape/order - which is what makes NATURAL (t=0.5,
    // where both bands' gain is 1.0) a true identity transform even
    // though the filter is always running: lowGain*sideLow +
    // highGain*sideHigh == sideLow + sideHigh == side when both gains
    // are 1.0, regardless of the crossover's own frequency response.
    inline constexpr float panCrossoverHz = 180.0f;

    // ---- Smoothing -------------------------------------------------------
    inline constexpr double panSmoothingSeconds = 0.02;
}
