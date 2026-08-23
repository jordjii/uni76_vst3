#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <memory>
#include <vector>

#include "Biquad.h"

/*
    UNI 76's PITCH / VARISPEED module - the fourth real DSP in the plugin
    (after PREAMP, EQ and SAT). PAN/VERB/IMAGE stay passthrough - see
    CLAUDE.md and docs/DSP_PITCH.md.

    Pure pitch-shift only - duration is always preserved (every process()
    call hands the underlying engine equal input/output sample counts, so
    there is never any time-stretching): no saturation, no coloration, no
    modulation. Wraps two fully independent mono instances of the vendored
    Signalsmith Stretch engine (ThirdParty/signalsmith-stretch, MIT) - one
    per channel, deliberately not one shared multi-channel instance, so
    that bit-identical stereo input is guaranteed (by construction, not by
    luck) to produce bit-identical stereo output. See docs/DSP_PITCH.md's
    "Stereo coherence" section for the measured evidence behind that
    choice. The vendored engine itself is kept out of this header (PIMPL)
    so this file doesn't leak ThirdParty/ include paths into every
    consumer (notably Tests/PluginTests.cpp).

    Realtime-safety contract matches PreampProcessor/EqProcessor/
    SatProcessor: prepare() is the only place that allocates; process()
    never allocates, locks, or touches the filesystem/WebView -
    SignalsmithStretch::configure() (called only from prepare()) is the
    engine's own allocation point, and setTransposeSemitones()/process()
    (called every block) allocate nothing per the upstream API contract.
*/

namespace uni76::dsp
{
    class PitchProcessor
    {
    public:
        PitchProcessor();
        ~PitchProcessor();

        void prepare (double sampleRate, int maximumBlockSize, int numChannelsToUse);
        void reset() noexcept;

        /** semitones is the already-discrete APVTS value (-12..+12,
            integer-valued but passed as int), clamped defensively here
            too. enabled drives the latency-aligned bypass crossfade, same
            pattern as every other module. */
        void process (juce::AudioBuffer<float>& buffer, int semitones, bool enabled) noexcept;

        /** Constant for the lifetime of a prepare() call - depends only on
            sample rate (STFT block/interval configuration), never on the
            semitone value or enabled state, so the host's plugin-delay-
            compensation stays valid throughout - see docs/DSP_PITCH.md's
            "Fixed latency" section. */
        int getLatencySamples() const noexcept { return latencySamples; }

    private:
        struct Engine;
        std::unique_ptr<Engine> engine;

        static constexpr int maxChannels = 2;

        double sampleRate = 44100.0;
        int latencySamples = 0;
        int numChannels = 2;

        std::array<IntegerDelayLine, maxChannels> dryDelays;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassSmoother;
        std::vector<float> bypassRampScratch;

        juce::AudioBuffer<float> dryScratch;
        juce::AudioBuffer<float> wetScratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchProcessor)
    };
}
