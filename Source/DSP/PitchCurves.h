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

    inline constexpr double pitchBypassSmoothingSeconds = 0.02;
}
