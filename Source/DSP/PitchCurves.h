#pragma once

/*
    Single source of truth for PITCH / VARISPEED's tuned constants. See
    docs/DSP_PITCH.md for the full configuration benchmark this STFT
    block/interval pair was chosen from (bass-stability was the deciding
    metric, not the lowest latency candidate - see "Configuration
    benchmark" in that document).
*/

namespace uni76::dsp
{
    // Signalsmith Stretch block/interval, as a fraction of sample rate.
    //
    // 140ms block / 35ms interval measured clearly more stable low-
    // frequency tracking (lower frequency deviation, lower amplitude-
    // modulation depth, better sideband suppression) than the library's
    // own presetDefault() (120ms/30ms) across a 40-120Hz x +/-12 semitone
    // sweep - see docs/DSP_PITCH.md for the full measured table.
    //
    // A shortened window (100ms/25ms) was tried in an earlier round on
    // the theory that it would reduce phase-vocoder smearing ("room"
    // character) and that the bass-stability headroom documented above
    // could pay for the tradeoff. It could not: on polyphonic material
    // the shorter window stopped resolving closely-spaced partials at
    // all (a 120Hz partial at -7ST landed on 97.4Hz instead of 80.1Hz,
    // 21.7% error) - audibly wrong notes, not a subtle drift, on exactly
    // the guitar-chord material this plugin is aimed at. Reverted to the
    // benchmarked 140ms/35ms. The window length is a genuine, unresolved
    // latency/smearing tradeoff for a future pass with a materially
    // different STFT front-end, not something this round's stereo-
    // architecture fix (see PitchProcessor.h) could address - the "room"
    // character measured before this round turned out to be dominated by
    // the independent-per-channel-phase mechanism, not window smearing
    // alone (see docs/DSP_PITCH.md's "Stereo coherence" section).
    inline constexpr double pitchStftBlockSeconds = 0.14;
    inline constexpr double pitchStftIntervalSeconds = 0.035;

    // Signalsmith Stretch's own "tonality limit" feature - a non-linear
    // frequency map that preserves more of the original timbre/phase
    // coherence above this frequency, instead of remapping every bin's
    // frequency linearly by the full transpose ratio. Lower than the
    // library's own documented example value (8000Hz) - lowering it
    // hands a larger share of the spectrum back to coherent, unsmeared
    // resynthesis, which is exactly the upper-mid/treble region where
    // phase incoherence reads as diffuse space rather than as pitch.
    // Costs nothing in low-frequency resolution: the bass-stability
    // benchmark's whole 40-120Hz range sits far below the limit either
    // way. Passed to setTransposeSemitones()'s second argument in
    // PitchProcessor.cpp as pitchTonalityLimitHz / sampleRate (the API
    // takes it normalised against sample rate, not a raw Hz value) - see
    // the library's own setTransposeFactor() comment.
    inline constexpr double pitchTonalityLimitHz = 4000.0;

    inline constexpr double pitchBypassSmoothingSeconds = 0.02;
}
