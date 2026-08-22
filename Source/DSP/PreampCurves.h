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
    inline constexpr float preampDriveGainMax = 20.0f;
    inline constexpr float preampDriveShapeExponent = 1.3f;

    inline float preampDriveGainLinear (float driveNormalised01) noexcept
    {
        const auto t  = clamp01 (driveNormalised01);
        const auto tt = std::pow (t, preampDriveShapeExponent);
        return preampDriveGainMin * std::pow (preampDriveGainMax / preampDriveGainMin, tt);
    }

    // Quadratic (even-harmonic) asymmetry mixed into the waveshaper's
    // argument, scaled by drive so H2 is negligible near DRIVE=0 and a
    // clear part of the character by DRIVE=100, without ever dominating
    // the (dominant) odd-order content from tanh() itself.
    inline constexpr float preampAsymmetryMax = 0.18f;

    inline float preampAsymmetryAmount (float driveNormalised01) noexcept
    {
        return preampAsymmetryMax * clamp01 (driveNormalised01);
    }

    // Output trim on top of the waveshaper's own tanh-normalisation.
    // Dividing by tanh(driveGain) keeps small signals close to unity gain
    // only while driveGain*x stays small - at DRIVE=100 with a typical
    // program-level input, driveGain*x is deep in the saturating region,
    // and tanh-normalisation alone measured ~+16 dB of RMS growth from
    // DRIVE=0 to DRIVE=100 (see docs/DSP_PREAMP.md). This trim uses the
    // same shaping exponent as the drive curve itself so it tracks (and
    // largely cancels) that growth, keeping DRIVE=100 from reading as
    // simply "a lot louder" than DRIVE=0 without fully loudness-
    // normalising every setting.
    inline constexpr float preampOutputTrimMaxDb = -15.0f;

    inline float preampOutputCompensationDb (float driveNormalised01) noexcept
    {
        const auto t = clamp01 (driveNormalised01);
        return preampOutputTrimMaxDb * std::pow (t, preampDriveShapeExponent);
    }

    // ---- Transformer coloration (pre-nonlinearity) -----------------------
    //
    // One-pole "core" rounding filter ahead of the waveshaper - lower
    // cutoff softens/rounds transients and reduces alias-prone HF energy
    // reaching the nonlinearity. Fixed-frequency low-shelf adds low-mid
    // density, magnitude scaled by drive.
    inline constexpr float preampRoundingCutoffMaxHz = 20000.0f;
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
