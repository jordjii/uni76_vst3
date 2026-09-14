#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

#include "Biquad.h"
#include "DelayCurves.h"

/*
    UNI 76's DELAY module - the 8th real DSP in the plugin (added
    2026-09-14, alongside PREAMP/EQ/SAT/PITCH/PAN/VERB/IMAGE) - see
    CLAUDE.md and docs/DSP_DELAY.md.

    A tempo-synced (never free-running-millisecond) echo, warm and
    vintage rather than clean/digital:

        MIX (`delay`, 0..100%, default 0%)      - dry/wet balance only.
        FEEDBACK (`delayFeedback`, 0..95%, default 30%) - per-pass loop gain.
        DIVISION (`delayDivision`, 5 choices, default "1/8") - BPM-locked time.
        STEREO (`delayStereo`, bool, default false/MONO).
        PING PONG (`delayPingPong`, bool, default false/OFF).

    Topology - Dry stays completely clean at every setting, including
    MIX=0% (an exact identity):

        DI/DRY --------------------------------------------------> MIX
        INPUT (mono-summed for the wet send in MONO/PING-PONG modes,
               kept per-channel in STEREO mode - see process())
          -> delay line(s) (two independent buffers, L and R - see
             "Mode routing" below for how each mode wires them)
          -> per-pass feedback-path tone shaping (soft ~100Hz highpass,
             progressive ~3.2kHz-corner one-pole lowpass damping so every
             successive repeat is darker than the last, a tiny bounded
             tanh saturation) - this SAME tone-shaped tap is both what
             the user hears (the "repeat") and what re-enters the buffer
             (scaled by FEEDBACK) for the next regeneration, so FEEDBACK=0%
             still gives one full-volume, tone-shaped repeat with no
             further regeneration, matching the product brief's own
             "0% = one audible repeat, no regeneration" spec exactly.
          -> MIX (dry*(1-mix) + wetProcessed*mix - a genuine crossfade,
             not an aux-send: "MIX controls only the Dry/Wet ratio" per
             the product brief, so at MIX=0% dry passes through at
             exactly unity gain and at MIX=100% only the processed tap is
             heard)
          -> Output

    **Mode routing** (`process()`):
      - MONO: a single buffer (bufferL), fed from the mono-summed input,
        self-feeding its own feedback path. Both output channels receive
        the identical wet tap (real L/R identical, per the product brief).
      - STEREO: two independent buffers, each fed from its OWN input
        channel and its OWN feedback path - no cross-talk, same BPM
        division on both (there is only one time/feedback pair of
        coefficients in this class at all - a per-channel time offset is
        not representable, which is itself the guarantee L/R can never
        drift to different times).
      - PING PONG: two buffers again, but cross-fed - bufferL is written
        from the mono-summed input PLUS bufferR's own feedback tap;
        bufferR receives ONLY bufferL's feedback tap (no direct input at
        all). This is the classic single-chain ping-pong topology (input
        enters once, on the Left side by design - see docs/DSP_DELAY.md -
        then alternates sides every further regeneration), not two
        independently-triggered lines, which is what actually produces
        genuine L-R-L-R alternation on a mono/centred source rather than
        a doubled simultaneous hit on both sides.
      - Per the product brief's own explicit conflict rule: PING PONG
        being on always forces the *effective* processing mode to stereo/
        ping-pong, regardless of what the raw `delayStereo` parameter
        currently holds (a DAW automation lane could otherwise leave them
        in a "PING PONG on, MONO" combination) - resolved once per block
        from the two boolean inputs, never by mutating either parameter
        from the audio thread.

    **Click-free BPM/division changes** (see DelayCurves.h's "Click-free
    time changes" section): two independent, constant-time read taps
    ("voices") into the same buffer(s), crossfaded over ~50ms whenever the
    target time actually changes - the active voice's own pitch is never
    bent (unlike sweeping a single read pointer's speed), only which of
    two fixed-time taps is currently audible changes, gradually.

    Realtime-safety contract matches every other module: prepare() is the
    only place that allocates (the delay-line buffers, sized for the
    actual sample rate and the fixed `delayMaxTimeSeconds` safety bound);
    process() never allocates, locks, or touches the filesystem/WebView.
*/

namespace uni76::dsp
{
    class DelayProcessor
    {
    public:
        DelayProcessor() = default;

        void prepare (double sampleRate, int maximumBlockSize, int numChannelsToUse);
        void reset() noexcept;

        /** mixNormalised01, feedbackNormalised01, divisionIndex, stereo,
            pingPong, hostBpm and enabled are all read once per call - mix/
            feedback are smoothed internally (~20ms), so passing a raw
            (possibly jumpy) automation value each block is safe and
            expected. divisionIndex selects one of DelayCurves.h's 5 fixed
            note divisions, resolved against hostBpm (read once per block
            from AudioPlayHead - see PluginProcessor.cpp, same pattern
            PAN's own tempo-synced RATE knob already established). Mono
            buses (numChannels < 2) are always processed as MONO
            regardless of `stereo`/`pingPong` - there is no second channel
            to be stereo/ping-pong across. */
        void process (juce::AudioBuffer<float>& buffer, float mixNormalised01, float feedbackNormalised01,
                       int divisionIndex, bool stereo, bool pingPong, double hostBpm, bool enabled) noexcept;

        /** Always 0 - the delay line's own buffered content is a wet-path
            effect, not a lookahead/analysis delay applied to the direct
            signal (the dry path is never touched by it), so there is
            nothing here a host needs to compensate for - same reasoning
            VERB's own pre-delay/tank already document. */
        int getLatencySamples() const noexcept { return 0; }

    private:
        double sampleRate = 44100.0;
        int numChannels = 2;

        // ---- delay buffers (always both allocated; which ones are used
        // and how they're wired together depends on the resolved mode -
        // see the class comment's "Mode routing" section) ----
        std::vector<float> bufferL, bufferR;
        int writePosL = 0, writePosR = 0;
        int bufferCapacitySamples = 1;

        // ---- click-free time changes: two crossfaded constant-time taps
        // (see DelayCurves.h's "Click-free time changes" section) - one
        // shared pair of voice times for both channels (L/R always share
        // the same BPM division - see the class comment). ----
        std::array<float, 2> voiceTimeSamples { 0.0f, 0.0f };
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> voiceCrossfade;

        // ---- feedback-path tone shaping - independent per-channel state,
        // since STEREO mode runs two genuinely separate feedback loops.
        Biquad feedbackHighpassL, feedbackHighpassR;
        OnePoleLowPass feedbackDampingL, feedbackDampingR;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> feedbackSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassSmoother;

        // Unlike VERB's own additive-send shape, MIX here is a genuine
        // crossfade (dry*(1-mix) + wet*mix) - the product brief is
        // explicit that MIX controls "only the Dry/Wet ratio". Dry is
        // still never passed through the delay line/feedback filters/
        // saturation (only its final output gain is touched), so no dry
        // copy buffer is needed either way - dry is read into a local and
        // written back scaled, in the same per-sample loop iteration.

        float readInterpolated (const std::vector<float>& buf, float readPosSamplesBack, int writePos) const noexcept;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DelayProcessor)
    };
}
