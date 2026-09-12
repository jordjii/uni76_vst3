#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

#include "Biquad.h"
#include "VerbCurves.h"

/*
    UNI 76's VERB / VINTAGE SPACE module - the sixth real DSP in the
    plugin (after PREAMP, EQ, SAT, PITCH and PAN). Imager stays
    passthrough - see CLAUDE.md and docs/DSP_VERB.md.

    A single 1970s-style electromechanical PLATE reverb + analog send/
    return electronics - not a generic digital hall, not a ROOM/PLATE/
    CHAMBER morph, not a convolution IR. Every setting is the same plate
    machine; the macro only changes send amount, decay time, and
    pre-delay:

        0%   = DRY   - bit-exact (up to float rounding) identity.
        50%  = PLATE  - classic, dense, dark studio plate.
        100% = DEEP    - longer, deeper, still a usable insert effect.

    Topology (see docs/DSP_VERB.md for the full derivation and measured
    data):

        DRY ------------------------------------------------> MIX
        INPUT
          -> wet send HPF (350Hz, cascaded 4-pole Butterworth)
          -> analog send stage (asymmetric tanh, DRIVE-scaled - see
             VerbCurves.h's "DRIVE (nested knob)" section - tiny/
             "texture" at DRIVE=0%, up to genuinely hot at 100%)
          -> diffuser (4-stage short-delay allpass - early density)
          -> pre-delay (smoothly variable, linear-interpolated)
          -> FDN plate tank (12 delay lines, Householder feedback
             matrix, per-line one-pole damping so highs decay faster
             than mid, fixed decorrelated stereo output taps)
          -> analog return stage (asymmetric tanh, also DRIVE-scaled,
             + ~9.5kHz soft bandwidth ceiling)
          -> wet output HPF safety (350Hz, lighter, 2-pole)
          -> MIX (dry*(1-wet) + wetProcessed*wet)
          -> enable/disable crossfade against a dry copy (zero latency,
             no delay-alignment needed)
          -> Output

    Both wet-path highpasses exist specifically so that DRY bass/kick
    stays completely untouched (the dry path never passes through either
    filter) while the WET plate tank never receives or sustains
    meaningful sub/bass energy - see "350Hz wet-path isolation" in
    docs/DSP_VERB.md for why a single input-side filter alone is not
    trusted to guarantee this for a recirculating feedback network.

    `verbWetGain(0) == 0.0` exactly (VerbCurves.h) is what makes DRY
    provably (not just measured) a bypass: at t=0 the wet contribution
    is multiplied by exactly zero before it is ever added to the dry
    signal, regardless of what state the plate tank itself is in.

    Realtime-safety contract matches every other module: prepare() is
    the only place that allocates (the delay-line buffers, sized for the
    actual sample rate); process() never allocates, locks, or touches
    the filesystem/WebView.
*/

namespace uni76::dsp
{
    class VerbProcessor
    {
    public:
        VerbProcessor() = default;

        void prepare (double sampleRate, int maximumBlockSize, int numChannelsToUse);
        void reset() noexcept;

        /** wetNormalised01, driveNormalised01 and enabled are read once
            per call - all smoothed internally, so passing a raw (possibly
            jumpy) automation value each block is safe and expected.
            driveNormalised01 drives the nested DRIVE knob (see
            VerbCurves.h's "DRIVE (nested knob)" section) - at 0% it
            reproduces this module's original, pre-existing send/return
            coloration exactly. */
        void process (juce::AudioBuffer<float>& buffer, float wetNormalised01, float driveNormalised01, bool enabled) noexcept;

        /** Always 0 - the plate's pre-delay and recirculation are wet-
            path effects, not a lookahead/analysis delay on the direct
            signal, so there is nothing here a host needs to
            compensate for (same reasoning EQ/PAN already document). */
        int getLatencySamples() const noexcept { return 0; }

    private:
        double sampleRate = 44100.0;
        int numChannels = 2;

        // ---- wet-path bass isolation ----
        Biquad wetSendHighpassA, wetSendHighpassB;      // 4-pole (2x cascaded 2-pole) on the send
        Biquad wetOutputHighpassL, wetOutputHighpassR;  // lighter safety highpass on the wet output, per channel

        // ---- pre-delay (smoothly variable) ----
        std::vector<float> preDelayBuffer;
        int preDelayWritePos = 0;

        // ---- diffuser (early density, ahead of the tank) ----
        std::array<std::vector<float>, verbNumDiffusers> diffuserBuffers;
        std::array<int, verbNumDiffusers> diffuserWritePos {};

        // ---- FDN plate tank ----
        std::array<std::vector<float>, verbNumLines> lineBuffers;
        std::array<int, verbNumLines> lineWritePos {};
        std::array<int, verbNumLines> lineLengthSamples {};
        std::array<OnePoleLowPass, verbNumLines> lineDamping;
        std::array<float, verbNumLines> lineFeedbackGain {};

        // ---- tail chorus/vibrato (see VerbCurves.h's "Tail chorus/
        // vibrato" section) - a small per-line LFO wobbling each line's
        // own *read* position (not its write side or nominal length) via
        // linear interpolation between two adjacent buffer samples.
        std::array<double, verbNumLines> chorusLfoPhase {};
        std::array<double, verbNumLines> chorusLfoIncrement {};

        // ---- analog return bandwidth ----
        OnePoleLowPass returnBandwidthL, returnBandwidthR;

        // ---- breakup (envelope-inverse return-stage character, see
        // VerbCurves.h's "Breakup" section) ----
        float breakupLevelSmoothed = 0.0f;    // fast-ish smoothed |tank output| envelope
        float breakupLevelAlpha = 1.0f;       // one-pole coefficient, computed in prepare()
        float breakupPeakLevel = 0.0f;        // slow-decaying "recent loudest point" reference
        float breakupPeakDecayPerSample = 1.0f; // computed in prepare()

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassSmoother;

        // Unlike EQ/PAN (which replace the signal, so bypass crossfades
        // between a saved dry copy and the processed result), VERB is an
        // additive send effect: dry is read into locals and written back
        // unmodified at the top/bottom of the same per-sample loop
        // iteration - no dry copy buffer is needed at all, and the wet
        // contribution alone is what the bypass smoother scales to zero.
        // This makes "dry is never touched" an algebraic guarantee, not
        // just a measured property - see the class comment above.

        void updateDecayDependentCoefficients (float wetForCoefficients) noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VerbProcessor)
    };
}
