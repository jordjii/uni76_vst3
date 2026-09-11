#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <memory>
#include <vector>

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

    Enable/disable behaviour - a deliberate exception to the delay-aligned
    crossfade bypass every other latency-owning module (PREAMP, SAT) uses.
    Real user-reported bug: PITCH's STFT engine used to run continuously
    regardless of `enabled`, so the module's own ~140ms algorithmic
    latency was held open at all times - even in every factory preset
    (all 32, including "Default", leave PITCH disabled), a freshly-loaded
    preset still inflicted a genuinely noticeable, persistent monitoring
    delay on live MIDI/audio input through the plugin. PREAMP/SAT can
    afford a constant-latency bypass because their own always-on
    oversampling latency is a few samples (a fraction of a millisecond);
    PITCH's is 140ms, large enough that "always held open" is itself the
    defect. `process()` now genuinely skips the engine and reports (via
    the plugin's total latency, not this class's own getLatencySamples() -
    see below) zero added delay once settled disabled - a true bypass,
    not a delayed one. The tradeoff: enabling PITCH mid-playback is no
    longer a click-free, time-aligned crossfade (that requires the two
    signals - live dry and 140ms-lagged wet - to share a timeline, which
    a genuinely variable total latency makes impossible); the short
    (`pitchBypassSmoothingSeconds`) transition blends the STFT engine's
    output against the *live* (undelayed) input instead, and the engine
    itself needs its own ~140ms warm-up from a cold start before its
    output is representative - the same settle-in any time-based effect
    incurs when engaged from bypass. Toggling PITCH on/off while playing
    is expected to be rare; a persistent 140ms lag on every note for
    however long the module merely exists in the chain is not an
    acceptable trade against it.

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
            too. enabled drives a true (not delay-aligned) bypass - see
            the class comment's "Enable/disable behaviour" section. */
        void process (juce::AudioBuffer<float>& buffer, int semitones, bool enabled) noexcept;

        /** The engine's own algorithmic latency while it is actually
            running - constant for the lifetime of a prepare() call,
            depends only on sample rate (STFT block/interval
            configuration), never on the semitone value - see
            docs/DSP_PITCH.md's "Fixed latency" section. This is NOT the
            module's current real output delay: that is 0 while disabled
            (a true bypass, see the class comment's "Enable/disable
            behaviour" section) and this value once engaged.
            PluginProcessor::updateReportedLatency() is what combines this
            with the live `pitchEnabled` flag for the host-facing total. */
        int getLatencySamples() const noexcept { return latencySamples; }

    private:
        struct Engine;
        std::unique_ptr<Engine> engine;

        static constexpr int maxChannels = 2;

        double sampleRate = 44100.0;
        int latencySamples = 0;
        int numChannels = 2;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassSmoother;
        std::vector<float> bypassRampScratch;

        /** No longer a delay-line output (see the class comment) - just a
            raw, live copy of the input, captured before the engine call so
            it can be blended against during an enable/disable transition. */
        juce::AudioBuffer<float> dryScratch;
        juce::AudioBuffer<float> wetScratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchProcessor)
    };
}
