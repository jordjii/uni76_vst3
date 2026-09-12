#pragma once

#include <algorithm>
#include <cmath>

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
    // Brought back down from 2.6 (itself raised from 1.6 during an
    // earlier "hits like nothing" round) after direct feedback that the
    // combination read as "резко" (harsh/abrupt) once PanoramaProcessor
    // also gained a genuine Mid rotation (see PanoramaProcessor.cpp's own
    // comment). The two effects share the same theta swing and peak at
    // the *same instant* - at width=100%/MOTION's extreme, this gain
    // multiplies the already-constant-power Mid rotation's own peak
    // (sqrt2, a normal, expected hard-pan headroom figure) on top,
    // compounding into a real, audible overshoot (roughly +10dB above
    // the input's own level at the swing's peak with the old 2.6 value -
    // not a "sounds different" complaint, a genuine hot/peaky transient
    // once per LFO half-cycle). 1.6 keeps width clearly audible on its
    // own while leaving the *panning* intensity to Mid's own rotation,
    // which is now the more direct, better-behaved lever for "hits hard"
    // - see docs/DSP_PAN.md's "Correlation" section for this constant's
    // fuller history.
    inline constexpr float panWidthMaxHigh = 1.6f;
    inline constexpr float panWidthMaxLow  = 1.15f; // low band ceiling at t=1.0 - bass barely widens (unchanged - bass stays centred by design)

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
    // Raised to a full 1.0 (was 0.85) alongside panWidthMaxHigh/
    // panMotionThetaRange below - at width=100% the swing should reach the
    // theta range's own full extent, not 85% of it.
    inline constexpr float panMotionDepthMaxHigh = 1.0f;
    inline constexpr float panMotionDepthMaxLow  = 0.12f; // unchanged - bass motion stays close to inaudible by design

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
    // module's filter state).
    //
    // The clock's own *speed* used to be this fixed constant; it is now
    // driven by the nested RATE knob (`ParamID::panRate`, see
    // panRateHz() below) - kept here, unused by PanoramaProcessor
    // directly any more, purely as the documented anchor value RATE's own
    // default normalised position (see ParameterLayout.cpp) is solved
    // against, so every existing preset/session that never touches RATE
    // keeps exactly this speed.
    inline constexpr double panLfoRateHz = 0.3;

    // ---- Motion rate (nested RATE knob - live-testing follow-up round) ------
    //
    // SoundToys PanMan's own Rate knob was the explicit reference: slow
    // and considered at the low end, clearly tremolo-fast at the high
    // end, on a continuously-variable (not tempo-synced - that is a
    // separate, larger, not-yet-started follow-up needing host BPM
    // access) exponential taper - the same `g = min*(max/min)^t` shape
    // this project's drive curves already use, not a linear Hz sweep
    // (a linear sweep would spend almost the whole knob in the
    // "impossibly slow" region and compress every musically useful speed
    // into the last few percent).
    //
    // panRateMinHz (20s per cycle) is close enough to "the field barely
    // moves" that it reads as a deliberate, glacial pace rather than
    // "broken/stuck". panRateMaxHz (8Hz) is fast enough to read as clear
    // tremolo-like flutter - well short of anything that would alias or
    // beat unpleasantly against typical program material, and matches
    // the fast end of PanMan's own Rate range by ear/spec.
    inline constexpr double panRateMinHz = 0.05;
    inline constexpr double panRateMaxHz = 8.0;

    inline double panRateHz (float rateNormalised01) noexcept
    {
        const auto t = std::clamp (rateNormalised01, 0.0f, 1.0f);
        return panRateMinHz * std::pow (panRateMaxHz / panRateMinHz, (double) t);
    }

    // ---- Tempo-synced motion rate (redesign round) ---------------------------
    //
    // Replaces the free-running Hz sweep above with a fixed list of musical
    // note divisions, matching a real reference plugin's own Length control
    // (Cableguys ShaperBox's Pan module - "Beat" LFO mode, note-division
    // dropdown from 1/128 up to 32 bars) - direct feedback was that a
    // continuously-variable Hz knob doesn't let the motion actually lock to
    // a song's tempo the way a real rhythmic pan effect needs to. The RATE
    // knob (`ParamID::panRate`) keeps its existing type/range (0..100%
    // AudioParameterFloat - no schema bump, no new parameter) but is now
    // interpreted as a *quantized index* into this table rather than a
    // continuous Hz curve - see panRateDivisionCount below for the knob's
    // own discrete-step wiring (Resources/Web/app.js).
    //
    // beatsPerCycle is in quarter-note beats (the standard DAW tempo unit,
    // independent of time signature) - a "triplet" division fits 3 notes in
    // the space of 2 of the same name (2/3 the length); a "dotted" division
    // is 1.5x the length. Actual LFO speed is solved from the *host's*
    // current tempo (see panRateSyncedHz below), not baked in here.
    struct PanRateDivision
    {
        const char* label;
        double beatsPerCycle;
    };

    inline constexpr std::array<PanRateDivision, 23> panRateDivisions
    { {
        { "1/128",       0.03125  },
        { "1/64",        0.0625   },
        { "1/32",        0.125    },
        { "1/16T",       0.166667 },
        { "1/16",        0.25     },
        { "1/16d",       0.375    },
        { "1/8T",        0.333333 },
        { "1/8",         0.5      },
        { "1/8d",        0.75     },
        { "1/4T",        0.666667 },
        { "1/4",         1.0      },
        { "1/4d",        1.5      },
        { "1/2T",        1.333333 },
        { "1/2",         2.0      },
        { "1/2d",        3.0      },
        { "1 Bar",       4.0      },
        { "1.5 Bars",    6.0      },
        { "2 Bars",      8.0      },
        { "3 Bars",      12.0     },
        { "4 Bars",      16.0     },
        { "8 Bars",      32.0     },
        { "16 Bars",     64.0     },
        { "32 Bars",     128.0    },
    } };

    // Default lands on "1 Bar" (index 15) - a slow, considered, musically
    // legible cycle, matching the reference plugin's own default and this
    // module's original ~0.3Hz-at-120BPM ballpark (4 beats at 120BPM = 2s/
    // cycle = 0.5Hz; close enough in character - the whole point of this
    // redesign is that the exact old free-running speed no longer applies
    // once motion is tempo-locked, so this is a deliberate new anchor, not
    // a preserved one - see docs/DSP_PAN.md's "Motion rate" section).
    inline constexpr int panRateDivisionCount = (int) panRateDivisions.size();
    inline constexpr int panRateDefaultDivisionIndex = 15;
    inline constexpr float panRateDefaultNormalised =
        (float) panRateDefaultDivisionIndex / (float) (panRateDivisionCount - 1);

    /** Quantizes a raw 0..1 knob position to the nearest division index -
        the same step count the frontend knob snaps to (ParameterKnob's own
        `steps` option, knob.js), so a value read back mid-drag (before the
        host's own quantization round-trips through automation) still
        resolves to the same division the user is looking at. */
    inline int panRateDivisionIndex (float rateNormalised01) noexcept
    {
        const auto t = std::clamp (rateNormalised01, 0.0f, 1.0f);
        const auto index = (int) std::lround ((double) t * (double) (panRateDivisionCount - 1));
        return std::clamp (index, 0, panRateDivisionCount - 1);
    }

    inline const PanRateDivision& panRateDivisionAt (float rateNormalised01) noexcept
    {
        return panRateDivisions[(size_t) panRateDivisionIndex (rateNormalised01)];
    }

    // A non-finite or non-positive host tempo (some hosts report 0 before
    // transport ever starts, or don't report tempo at all in certain
    // configurations) falls back to a sane default rather than reaching
    // a division-by-zero or NaN below.
    inline constexpr double panRateFallbackBpm = 120.0;

    /** The actual LFO Hz for the current RATE knob position at the given
        host tempo - PanoramaProcessor.cpp's only call site. */
    inline double panRateSyncedHz (float rateNormalised01, double hostBpm) noexcept
    {
        const auto safeBpm = (std::isfinite (hostBpm) && hostBpm > 1.0) ? hostBpm : panRateFallbackBpm;
        const auto& division = panRateDivisionAt (rateNormalised01);
        return (safeBpm / 60.0) / division.beatsPerCycle;
    }

    // Equal-power motion: at depth=0, theta sits at the fixed centre
    // pi/4 (cos==sin==1/sqrt(2), i.e. the ordinary symmetric-width
    // gainL==gainR==1 case). As depth grows, theta swings further away
    // from that centre, following the LFO - see
    // PanoramaProcessor::process() for the constant-power proof
    // (gainL^2 + gainR^2 == 2 for *any* theta, algebraically, not just
    // measured) this angle-based formulation provides.
    //
    // Restored to a full pi/4 (a full quarter-turn swing at depth=1) for
    // the HIGH band specifically - an earlier round had reduced this to
    // 0.55 specifically to keep correlation from going negative on
    // correlated material, but direct listening feedback against a real
    // reference (SoundToys PanMan) was that the result read as "basically
    // nothing" at 100% width. This is a deliberate, explicit reversal of
    // that earlier priority for the HIGH band: audible ear-to-ear
    // intensity now wins over strict mono-correlation safety at the top
    // of the knob, the same trade-off panWidthMaxHigh above makes. At
    // depth=1 (with panMotionDepthMaxHigh now also 1.0), thetaHigh swings
    // the full [0, pi/2] range - gainHigh's L/R ratio reaches a genuinely
    // hard pan (one side's high-band spatial gain hits exactly 0) at the
    // LFO's extremes, not just a wide-but-never-silent ~8:1. See
    // docs/DSP_PAN.md's "Correlation" section for the updated measured
    // numbers this reopened.
    //
    // panMotionThetaRangeLow was SPLIT OUT from a single shared
    // panMotionThetaRange during this same intensity round - a real
    // regression found via direct listening feedback ("низкочастотный
    // прикол на больших значениях" - a low-frequency artefact at high
    // width): panMotionThetaRange used to scale *both* bands' theta
    // swing identically, so raising it for the high band's sake also
    // widened the LOW band's own rotation swing by the same ~43%
    // (0.55->0.785, the same ratio the high band's own widening used),
    // even though panMotionDepthMaxLow (the low band's own *depth*
    // ceiling, 0.12) was untouched - the two constants were coupled by
    // construction, not by design intent. Bass gets a dedicated, small,
    // independently-tunable swing range again (the project's own
    // long-standing "bass gets much smaller width/motion ceilings than
    // mid/high" rule - see the class comment above and docs/DSP_PAN.md's
    // "Centre-bass isolation" section, whose own measured numbers this
    // restores), fully decoupled from however aggressive the high band's
    // own swing gets in the future.
    inline constexpr float panMotionThetaCentre   = 0.7853981633974483f; // pi/4
    inline constexpr float panMotionThetaRangeLow  = 0.55f;               // bass: same safe swing the crossover/correlation-fix round tuned
    inline constexpr float panMotionThetaRangeHigh = 0.7853981633974483f; // pi/4 - full swing, theta covers [0, pi/2]

    // Mid's own rotation range (PanoramaProcessor.cpp's gainMidL/gainMidR -
    // see its class comment) - deliberately narrower than the high band's
    // own full-hard-pan swing above. Direct feedback ("резко" - harsh/
    // abrupt) traced to a real, meaningful difference from the reference
    // plugin's own demonstrated setting: Cableguys ShaperBox's Pan module
    // was shown in "Pan Mode: Balance 3dB" - a *bounded* pan law that
    // never lets either channel reach true silence, unlike a full
    // constant-power hard pan (theta reaching exactly 0 or pi/2, where
    // sin/cos of the other hits exactly 0). panMotionThetaRangeHigh's own
    // full pi/4 range does exactly that at full MOTION - fine for the
    // *width* shelf's own high-frequency asymptote (a widening effect,
    // not literally silencing a channel), but applied directly and
    // full-band to Mid (which the width shelf isn't), that same full
    // swing reads as a channel abruptly cutting out and back, not a
    // smooth throw. 0.5 keeps Mid's own swing clearly, strongly audible
    // (the whole point of giving Mid a rotation at all) while never
    // reaching full silence on either side - the quiet channel stays
    // measurably present throughout, avoiding the on/off character.
    inline constexpr float panMidRotationThetaRange = 0.5f;

    // ---- Frequency-dependent gain shelf (WIDTH + MOTION) --------------------
    //
    // Two independent low-shelf filters (one per output channel, see
    // PanoramaProcessor::process()) reshape the spatial signal directly,
    // rather than splitting it into low/high *bands* first and applying a
    // different gain to each before summing. Two earlier revisions used a
    // band-split-then-sum architecture (first a 2nd-order Butterworth
    // crossover, measured ~5.5% Side-gain overshoot; then a "gentler"
    // 1st-order crossover, measured ~2.5dB residual near 150-200Hz) - both
    // still had a genuine mathematical bump, not a tuning problem: any two
    // complementary bands built from a causal IIR split are phase-shifted
    // relative to each other (e.g. exactly 90 degrees apart at a 1st-order
    // crossover's own corner), so `a*low + b*high` (a != b) is a *vector*
    // sum, not a linear interpolation between a and b - it provably
    // overshoots both endpoints whenever a != b (see docs/DSP_PAN.md's
    // "Crossover artifact" section for the derivation and measured before/
    // after). A single shelf filter has no second, differently-gained path
    // to vector-sum against - by construction (RBJ cookbook shelf, S=1
    // "maximally flat" slope - no resonant peaking), its magnitude
    // response is a smooth, monotonic transition between its own low and
    // high asymptotes, so no overshoot is possible regardless of how far
    // apart those two asymptotes are. At 0dB gain (low asymptote == high
    // asymptote), Biquad.h's makeLowShelf collapses to an exact identity
    // filter (b0=a0, b1=a1, b2=a2 algebraically, not just approximately),
    // which is what makes ORIGINAL (t=0, every band gain exactly 1.0)
    // provably exact here, the same guarantee the previous band-split
    // design relied on its own complementary-subtraction identity for.
    inline constexpr float panCrossoverHz = 150.0f;
    inline constexpr float panShelfSlope  = 1.0f; // RBJ "S" - 1.0 is the maximally-flat, no-overshoot slope

    // ---- Induced-signal bass isolation --------------------------------------
    //
    // Corner frequency for `inducedHighpass` (PanoramaProcessor.cpp) - the
    // *proper*, independently-designed 2nd-order Butterworth highpass that
    // strips the synthesised "induced" signal's own bass content before it
    // is blended into the spatial signal. Deliberately its own named
    // constant, not reused from `panCrossoverHz` above: the two filters
    // play very different roles - `panCrossoverHz` shapes the width/motion
    // *shelves*, which must stay an exact identity at t=0 and therefore
    // must not be made arbitrarily steep (steepness there is a musical
    // trade-off against the shelf's own transition smoothness); this one
    // isolates a purely-additive branch with no reconstruction identity to
    // preserve at all, so it is free to be tuned purely for bass rejection.
    // A real Butterworth highpass (unlike the complementary-subtraction
    // `induced - LP(induced)` an earlier round used) has no phase-vector
    // hump right at its own corner - its magnitude is a clean, monotonic
    // -12dB/oct rolloff below `panInducedHighpassHz`, by construction.
    inline constexpr float panInducedHighpassHz = 150.0f;

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
