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
    // 140ms block / 35ms interval measured clearly more stable low-
    // frequency tracking (lower frequency deviation, lower amplitude-
    // modulation depth, better sideband suppression) than the library's
    // own presetDefault() (120ms/30ms) across a 40-120Hz x +/-12 semitone
    // sweep - see docs/DSP_PITCH.md for the full measured table. This
    // exceeds the product brief's "~50-80ms" latency guideline, which the
    // brief explicitly permits when bass stability measurably requires it.
    inline constexpr double pitchStftBlockSeconds = 0.14;
    inline constexpr double pitchStftIntervalSeconds = 0.035;

    // Signalsmith Stretch's own "tonality limit" feature (UX polish pass,
    // live-testing feedback: pitch-shifted material "smeared"/wobbled
    // audibly at large +/-ST amounts) - a non-linear frequency map that
    // preserves more of the original timbre/phase coherence above this
    // frequency, instead of remapping every bin's frequency linearly by
    // the full transpose ratio. 8000Hz is the library's own documented
    // example value (see ThirdParty/signalsmith-stretch/UPSTREAM_README.md's
    // "tonality limit" section) - a reasonable, conservative starting
    // point that leaves low/mid content (where the existing bass-
    // stability benchmark in docs/DSP_PITCH.md was measured, 40-120Hz)
    // untouched while reducing the audible "robotic"/wobble character in
    // the upper spectrum at extreme shift amounts. Passed to
    // setTransposeSemitones()'s second argument in PitchProcessor.cpp as
    // pitchTonalityLimitHz / sampleRate (the API takes it normalised
    // against sample rate, not a raw Hz value) - see the library's own
    // setTransposeFactor() comment. Not re-benchmarked against the
    // existing bass-stability/sideband-suppression measured tables this
    // pass - a follow-up, not done here.
    inline constexpr double pitchTonalityLimitHz = 8000.0;

    inline constexpr double pitchBypassSmoothingSeconds = 0.02;
}
