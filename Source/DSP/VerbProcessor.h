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

    ---------------------------------------------------------------------
    NEW DIRECTION (2026-09-14) - this is a from-scratch redesign. The
    module used to be a 1970s-style electromechanical PLATE reverb; that
    architecture, and plate as a sound reference at all, is retired - see
    CLAUDE.md's "VERB direction change" entry and docs/DSP_VERB.md's
    "Direction change" section for the full product brief. This is now a
    soft, warm, dark, VINTAGE ALGORITHMIC reverb with a chamber/hall
    character:

        - warm and mild, not clinical/digital-sounding;
        - a soft, dense tail, fixed at ~3 seconds regardless of Mix;
        - no metallic ringing, no fixed resonant/standing-out notes;
        - no hard/harsh early reflections, no audible comb filtering;
        - no explicit chorus and no pitch wobble anywhere in the tank;
        - slightly dark, sitting back/behind in the mix;
        - natural and musical even at Mix 70-100%.

    Deliberately NOT Dattorro's plate topology (which nests its own
    internal decay-diffusion allpasses inside two long delay "arms") and
    NOT a repair/retune of the old plate-era 16-line FDN. This is a plain
    N-line Feedback Delay Network (Stautner & Puckette 1982 style) fed by
    a long multi-stage input diffusion cascade, mixed with an orthogonal
    Householder matrix - a different line count, different lengths, and a
    much smaller, carefully-bounded modulation depth than the old plate
    module's own (see VerbCurves.h's "Tank line modulation" section for
    why a *small* amount of correctly-bounded modulation is the actual
    anti-metallic mechanism here, not something this redesign avoids -
    the old plate module's own never-fully-solved "metallic ring" history
    (see docs/DSP_VERB.md's superseded "Historical implementation"
    section) came from too little, unsafely-added modulation on a
    much-longer-decay tank, not from modulation being the wrong idea).

    Topology - **DRIVE lives entirely after the tank, on the wet tail
    only - it never touches the send, the DI/Dry signal, or the tank's
    own internal recirculation** (unchanged product contract from the
    plate era - see "DRIVE routing" below):

        DI/DRY ----------------------------------------------------> MIX
        INPUT
          -> wet send HPF (120Hz, cascaded 4-pole Butterworth)
          -> analog send stage (tiny, fixed asymmetric tanh - NOT
             DRIVE-scaled, always the same small "texture" coloration
             regardless of DRIVE, so the tank always receives an
             essentially clean signal and produces a clean, warm tail)
          -> input diffusion cascade (8-stage short-delay allpass -
             smooths the input into a dense wash BEFORE the tank, so
             there is no discrete early-reflection "slap")
          -> fixed pre-delay (16ms, constant regardless of Mix)
          -> reverb tank (12 delay lines, Householder feedback matrix,
             per-line one-pole damping so highs decay faster than mid, a
             small/slow/staggered per-line read-position modulation just
             large enough to detune the tank's own fixed resonant modes
             (the actual anti-"metallic ring" mechanism) but far below
             the depth where it would read as an audible chorus/pitch
             effect - see VerbCurves.h's "Tank line modulation" section -
             fixed decorrelated stereo output taps)
          -> analog return stage (asymmetric tanh, DRIVE-scaled here and
             only here - see "DRIVE routing" below - + static darkening
             bandwidth ceiling)
          -> DRIVE warmth/depth/width shaping (tail only, no modulation)
          -> wet output HPF safety (120Hz, lighter, 2-pole)
          -> MIX (dry*(1-wet) + wetProcessed*wet)
          -> enable/disable crossfade against a dry copy (zero latency,
             no delay-alignment needed)
          -> Output

    **DRIVE routing** (`VerbProcessor::process()`): the send stage's
    drive-gain/asymmetry constants are compile-time constants
    (`verbSendDriveGainBase`/`verbSendAsymmetryBase`) that never read
    `driveNormalised01` at all - only the return stage
    (`verbReturnDriveGain`/`verbReturnAsymmetry`, and the warmth/depth/
    width shaping after it) does. This means DI/Dry and the *signal the
    tank reverberates* are both completely independent of DRIVE by
    construction - DRIVE only ever overdrives the tank's own already-
    formed wet tail, never the input being sent into the tank, and never
    the tank's own recirculation/decay. Verified by dedicated null tests
    (`Tests/PluginTests.cpp`'s "DRIVE null test" and "DRIVE does not
    affect the tank's own decay/RT60") that confirm output is bit-
    identical across `verbDrive` 0%->100% whenever `reverb` (wet amount)
    is 0%, that the *dry* component specifically never moves with DRIVE
    at any wet amount, and that measured RT60 is unaffected by DRIVE.

    Both wet-path highpasses exist specifically so that DRY bass/kick
    stays completely untouched (the dry path never passes through either
    filter) while the WET tank never receives or sustains meaningful
    sub/bass energy - see docs/DSP_VERB.md.

    `verbWetGain(0) == 0.0` exactly (VerbCurves.h) is what makes DRY
    provably (not just measured) a bypass: at t=0 the wet contribution is
    multiplied by exactly zero before it is ever added to the dry signal,
    regardless of what state the tank itself is in.

    Mix (`reverb`) controls ONLY the dry/wet balance - decay time and
    pre-delay are fixed constants (verbTargetDecaySeconds/verbPreDelayMs
    in VerbCurves.h), never derived from Mix, per this round's explicit
    "Mix must not stretch Decay" requirement.

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
            VerbCurves.h's "DRIVE" section) - at 0% it reproduces this
            module's original, pre-existing send/return coloration
            exactly. */
        void process (juce::AudioBuffer<float>& buffer, float wetNormalised01, float driveNormalised01, bool enabled) noexcept;

        /** Always 0 - the tank's pre-delay and recirculation are wet-
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

        // ---- fixed pre-delay (constant - Mix never stretches this) ----
        IntegerDelayLine preDelay;

        // ---- input diffusion cascade (early density, ahead of the tank) ----
        std::array<std::vector<float>, verbNumDiffusers> diffuserBuffers;
        std::array<int, verbNumDiffusers> diffuserWritePos {};

        // ---- reverb tank: decorrelated delay lines, small bounded modulation ----
        std::array<std::vector<float>, verbNumLines> lineBuffers;
        std::array<int, verbNumLines> lineWritePos {};
        std::array<int, verbNumLines> lineLengthSamples {};
        std::array<OnePoleLowPass, verbNumLines> lineDamping;
        std::array<float, verbNumLines> lineFeedbackGain {};

        // Small, slow, per-line-staggered read-position modulation - see
        // VerbCurves.h's "Tank line modulation" section for why this is
        // present at all (it is the actual anti-metallic mechanism, not
        // a texture) and why it is kept far below the depth where it
        // would read as an audible chorus/pitch effect. Read via
        // AllpassFractionalDelay (Biquad.h), not plain linear
        // interpolation - a true allpass, so the modulation costs no
        // measurable RT60/level the way linear interpolation's own
        // frequency-dependent attenuation would (see Biquad.h's class
        // comment and its own isolated unit tests,
        // Tests/PluginTests.cpp's "uni76::dsp::AllpassFractionalDelay"
        // suite, verified BEFORE this was wired in here).
        std::array<double, verbNumLines> lineModPhase {};
        std::array<double, verbNumLines> lineModIncrement {};
        std::array<AllpassFractionalDelay, verbNumLines> lineInterpolators;

        // ---- analog return bandwidth (static darkening, not DRIVE-scaled) ----
        OnePoleLowPass returnBandwidthL, returnBandwidthR;

        // ---- DRIVE warmth (return-stage pre-emphasis, see VerbCurves.h's
        // "DRIVE warmth" section) ----
        Biquad returnWarmthShelfL, returnWarmthShelfR;

        // ---- DRIVE depth: extra, DRIVE-scaled bandwidth ceiling on the
        // driven tail, on top of the fixed returnBandwidth* pair above -
        // see VerbCurves.h's "Driven-tail placement" section.
        OnePoleLowPass driveDepthLowpassL, driveDepthLowpassR;

        // ---- DRIVE width: a small, FIXED (non-modulated) inter-channel
        // delay on the R return channel, blended in proportion to DRIVE -
        // decorrelates L/R without any LFO/chorus (see VerbCurves.h's
        // "Driven-tail placement" section for why this is static, unlike
        // the old plate module's modulated driven-tail chorus).
        IntegerDelayLine driveWidthDelayR;

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

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VerbProcessor)
    };
}
