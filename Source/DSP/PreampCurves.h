#pragma once

#include <algorithm>
#include <cmath>

/*
    Single source of truth for how the PREAMP / DRIVE parameter (0..1)
    maps onto every drive-dependent shape inside Source/DSP/PreampProcessor.

    These are pure, deterministic functions (no state, no allocation) so
    Tests/PluginTests.cpp can assert on them directly, and so
    Source/DSP/PreampProcessor.cpp has exactly one place to look up "what
    does the curve actually do at t=0.5".

    Resources/Web/aux_visuals.js hand-mirrors preampLowCutHz/preampHighCutHz
    for the purely-cosmetic LOW CUT / HIGH CUT indicator lines - there is no
    shared runtime between the plugin core and the WebView UI, so that copy
    is a literal transcription of the same formula, not a live binding. See
    docs/DSP_PREAMP.md for the measured curve values and the JS comment
    pointing back here.
*/

namespace uni76::dsp
{
    inline float clamp01 (float t) noexcept
    {
        return std::clamp (t, 0.0f, 1.0f);
    }

    // ---- Nonlinear stage drive ------------------------------------------
    //
    // The waveshaper normalises by tanh(driveGain) (see PreampProcessor),
    // so driveGain itself - not a separate wet/dry mix - is what carries
    // the module from "practically transparent" to "strong analogue
    // saturation". driveGainMin is deliberately tiny (~-26 dB): for small
    // g, tanh(g*x)/tanh(g) ~ x, i.e. structurally near-identity at DRIVE=0.
    // driveGainMax (~+26 dB) is where tanh(driveGain) has already
    // saturated to ~1, giving a firm but soft ceiling at DRIVE=100.
    inline constexpr float preampDriveGainMin = 0.05f;
    inline constexpr float preampDriveGainMax = 10.0f;
    // Exponent < 1 front-loads the curve. It used to be 1.3, which
    // *back*-loaded it, and that was a real, measurable bug found by live
    // testing ("turning the knob does nothing"): tanh() is linear to
    // within a fraction of a percent for arguments below ~0.2, and the
    // old curve only reached driveGain=0.43 at DRIVE=50%, i.e. an
    // argument of 0.215 on a -6dBFS peak. Measured THD across the knob
    // was 0.01 / 0.03 / 0.38 / 6.2 / 33.3 % at 0/25/50/75/100% - the
    // entire lower half of the control was an audible no-op, and all the
    // character was crammed into the last quarter. (docs/DSP_PREAMP.md's
    // own "close to flat through DRIVE=50% (0, -0.2, 0.1 dB)" line had
    // recorded exactly this and read it as correct behaviour.) At 0.6
    // the same endpoints now measure 0.01 / 0.52 / 4.85 / 18.8 / 33.3 %
    // - a continuous, musical progression where every part of the knob
    // does something. Endpoints are unchanged, so DRIVE=0% is still the
    // same near-identity and DRIVE=100% the same ceiling.
    inline constexpr float preampDriveShapeExponent = 0.6f;

    inline float preampDriveGainLinear (float driveNormalised01) noexcept
    {
        const auto t  = clamp01 (driveNormalised01);
        const auto tt = std::pow (t, preampDriveShapeExponent);
        return preampDriveGainMin * std::pow (preampDriveGainMax / preampDriveGainMin, tt);
    }

    // Fractional gain reduction applied to the waveshaper's negative half
    // only (see PreampProcessor.cpp) - a milder tanh() on one side of the
    // wave than the other, which is what actually produces even-harmonic
    // (H2) content from a per-half-bounded tanh(). Scaled by drive so H2
    // is negligible near DRIVE=0 and a clear part of the character by
    // DRIVE=100, without ever dominating the (dominant) odd-order content.
    //
    // Raised from 0.20 to 0.32 (live-testing follow-up, a deliberate
    // exception to this module's "frozen, don't change without a
    // discovered objective regression" status - see CLAUDE.md and
    // docs/DSP_PREAMP.md's "Tube-character follow-up" section) - real
    // single-ended-triode tube preamps typically show a more pronounced
    // even-harmonic (H2) dominance than the original, more conservative
    // figure gave; still well inside the safety margin the original
    // calibration pass proved (that pass found >400% THD/"fuzz" only
    // from a fundamentally different *additive* asymmetry model, not
    // from this per-half-bounded tanh() approach at any asymmetry up to
    // 1.0 - see the comment in PreampProcessor.cpp).
    inline constexpr float preampAsymmetryMax = 0.32f;

    inline float preampAsymmetryAmount (float driveNormalised01) noexcept
    {
        return preampAsymmetryMax * clamp01 (driveNormalised01);
    }

    // Output trim on top of the waveshaper's own tanh-normalisation.
    // Dividing by tanh(driveGain) keeps small signals close to unity gain
    // only while driveGain*x stays small - measured raw (untrimmed) RMS
    // growth at -18dBFS is close to flat through DRIVE=50% (0, -0.2, 0.1
    // dB) and then rises sharply toward DRIVE=100% (+5.2dB at 75%,
    // +16.8dB at 100% - see docs/DSP_PREAMP.md). A trim sharing the
    // drive curve's own (much gentler) exponent used to badly mismatch
    // that shape - it over-trimmed the already-near-flat 25-50% region
    // (measured -6dB dip there) while barely denting the real growth up
    // top. This curve's own steep exponent instead stays close to 0dB
    // until deep into the top of the range, then cancels most (not all)
    // of the measured growth - net RMS change at -18dBFS stays within
    // roughly +/-2dB through DRIVE=75% and about +2dB at DRIVE=100%,
    // deliberately leaving a little natural growth rather than acting as
    // a hard loudness normaliser.
    // Retuned alongside the drive curve above (same live-testing round).
    // The old -15dB/exponent-4 pair was cancelling almost all of the
    // level growth exactly where the saturation finally started, so the
    // module both distorted late *and* got quieter for it - the two
    // effects compounded into "nothing happens". A real preamp driven
    // harder gets louder as well as denser; that is the effect, not a
    // defect to normalise away. Now a much gentler curve that leaves the
    // growth clearly audible: measured net gain (raw growth + this trim)
    // is about +0.3/+3.4/+8.7/+11.4 dB at 25/50/75/100% on a -18dBFS
    // sine, and +0.2/+2.2/+3.8/+2.0 dB on a -6dBFS one (louder input
    // saturates sooner, so it grows less - correct, level-dependent
    // behaviour, not something the trim should flatten).
    inline constexpr float preampOutputTrimMaxDb = -6.0f;
    inline constexpr float preampOutputTrimExponent = 2.0f;

    inline float preampOutputCompensationDb (float driveNormalised01) noexcept
    {
        const auto t = clamp01 (driveNormalised01);
        return preampOutputTrimMaxDb * std::pow (t, preampOutputTrimExponent);
    }

    // ---- "Sag" - subtle program-dependent gain reduction (tube power-
    // supply sag) ---------------------------------------------------------
    //
    // New (live-testing follow-up, same deliberate frozen-module
    // exception as the asymmetry change above) - a real tube preamp's
    // plate voltage measurably sags under sustained/loud transient
    // content, which acts as a soft, musical, program-dependent gain
    // reduction distinct from - and much gentler/slower than - SAT's own
    // dedicated "glue" compression (SatCurves.h's satCompressionStrength,
    // 90ms release). Deliberately background-level here: max strength is
    // roughly 1/6 of SAT's own ceiling, and the release is over 2x
    // slower (a "breathing" quality, not a pumping compressor). Applied
    // ahead of the waveshaper (see PreampProcessor.cpp) via the exact
    // same bounded `1/(1+strength*envelope)` form SAT already uses (see
    // SatCurves.h) - proven safe/bounded for any non-negative envelope.
    // Scaled by drive so it's inert at DRIVE=0% (matching the module's
    // "structurally near-identity at 0" contract).
    inline constexpr float preampSagStrengthMax = 0.35f;
    inline constexpr float preampSagReleaseMs = 220.0f;

    inline float preampSagStrength (float driveNormalised01) noexcept
    {
        return preampSagStrengthMax * clamp01 (driveNormalised01);
    }

    // ---- Transformer coloration (pre-nonlinearity) -----------------------
    //
    // One-pole "core" rounding filter ahead of the waveshaper - lower
    // cutoff softens/rounds transients and reduces alias-prone HF energy
    // reaching the nonlinearity. Fixed-frequency low-shelf adds low-mid
    // density, magnitude scaled by drive.
    // Max (DRIVE=0%) cutoff is deliberately far above the audible band,
    // not just "20kHz" - a one-pole filter's magnitude response is still
    // measurably down almost an octave below its nominal cutoff (a real
    // -1dB null-test deviation was measured at 10kHz with a 20kHz
    // cutoff), and this filter is meant to be a *drive-dependent*
    // character effect, negligible at DRIVE=0%, not a fixed top-end trim.
    inline constexpr float preampRoundingCutoffMaxHz = 40000.0f;
    inline constexpr float preampRoundingCutoffMinHz = 9000.0f;
    inline constexpr float preampRoundingExponent = 0.85f;

    inline float preampRoundingCutoffHz (float driveNormalised01) noexcept
    {
        const auto t = clamp01 (driveNormalised01);
        return preampRoundingCutoffMaxHz
               - (preampRoundingCutoffMaxHz - preampRoundingCutoffMinHz) * std::pow (t, preampRoundingExponent);
    }

    inline constexpr float preampColorShelfFreqHz = 150.0f;
    inline constexpr float preampColorShelfMaxGainDb = 2.5f;

    inline float preampColorShelfGainDb (float driveNormalised01) noexcept
    {
        return preampColorShelfMaxGainDb * clamp01 (driveNormalised01);
    }

    // ---- Low Cut / High Cut (drive-dependent, internal - not a param) ---
    //
    // DRIVE 0%  -> Low Cut ~20 Hz,   High Cut ~20 kHz  (wide open)
    // DRIVE 50% -> Low Cut ~35-40Hz, High Cut ~15-16kHz
    // DRIVE 100%-> Low Cut ~65-75Hz, High Cut ~10-12kHz
    // See docs/DSP_PREAMP.md for the actual measured sweep.
    inline constexpr float preampLowCutMinHz = 20.0f;
    inline constexpr float preampLowCutMaxHz = 70.0f;
    inline constexpr float preampLowCutExponent = 1.4f;

    inline float preampLowCutHz (float driveNormalised01) noexcept
    {
        const auto t = clamp01 (driveNormalised01);
        return preampLowCutMinHz + (preampLowCutMaxHz - preampLowCutMinHz) * std::pow (t, preampLowCutExponent);
    }

    inline constexpr float preampHighCutMinHz = 11000.0f;
    inline constexpr float preampHighCutMaxHz = 20000.0f;
    inline constexpr float preampHighCutExponent = 1.15f;

    inline float preampHighCutHz (float driveNormalised01) noexcept
    {
        const auto t = clamp01 (driveNormalised01);
        return preampHighCutMaxHz - (preampHighCutMaxHz - preampHighCutMinHz) * std::pow (t, preampHighCutExponent);
    }

    // ---- Smoothing / bypass ----------------------------------------------
    inline constexpr double preampDriveSmoothingSeconds  = 0.025;
    inline constexpr double preampBypassSmoothingSeconds = 0.02;

    // ---- DC / infrasonic protection (fixed, not drive-dependent) --------
    inline constexpr float preampDcBlockerHz = 5.0f;
}
