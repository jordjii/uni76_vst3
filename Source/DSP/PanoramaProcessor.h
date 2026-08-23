#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "Biquad.h"

/*
    UNI 76's PAN / STEREO FIELD module - the fifth real DSP in the plugin
    (after PREAMP, EQ, SAT and PITCH). VERB/IMAGE stay passthrough - see
    CLAUDE.md and docs/DSP_PAN.md.

    NOT an L/R balance pan, despite the `panorama` parameter ID (kept only
    for compatibility - never renamed). It is a stereo *width* control:
    MONO (0%) <- NATURAL (50%) -> WIDE (100%). Mid/Side matrix, frequency-
    dependent Side gain (bass widens far less than mid/high - see
    PanoramaCurves.h), Mid channel always passed through completely
    untouched - which is what gives this module two provable, not just
    measured, guarantees: mono fold-down ((L+R)/2) is bit-identical
    before/after at every width setting (Mid is never modified), and
    NATURAL (50%) is a true identity transform even though the crossover
    filter is always running (see PanoramaCurves.h's class comment for
    why). No delay-based widening, no chorus, no random modulation, no
    saturation - a pure gain/filter morph, zero added latency.

    Realtime-safety contract matches PreampProcessor/EqProcessor/
    SatProcessor/PitchProcessor: prepare() is the only place that
    allocates; process() never allocates, locks, or touches the
    filesystem/WebView.
*/

namespace uni76::dsp
{
    class PanoramaProcessor
    {
    public:
        PanoramaProcessor() = default;

        void prepare (double sampleRate, int maximumBlockSize, int numChannelsToUse);
        void reset() noexcept;

        /** widthNormalised01 and enabled are read once per call - both are
            smoothed internally (~20ms), so passing a raw (possibly jumpy)
            automation value each block is safe and expected. Mono buses
            (numChannels < 2) are left untouched - there is no stereo
            field to widen, and this module never fabricates one. */
        void process (juce::AudioBuffer<float>& buffer, float widthNormalised01, bool enabled) noexcept;

        /** Always 0 - PAN is a pure gain/filter morph with no oversampling,
            no lookahead, no delay-based widening. Constant for the
            lifetime of a prepare() call, like every other module's
            getLatencySamples(). */
        int getLatencySamples() const noexcept { return 0; }

    private:
        double sampleRate = 44100.0;
        int numChannels = 2;

        // Splits the Side signal (mono - one derived signal, not one per
        // channel) into low/high bands - see PanoramaCurves.h.
        Biquad sideLowpass;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthSmoother;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassSmoother;

        // No latency to align against (getLatencySamples() == 0), so the
        // dry copy needs no delay line - same EQ-style bypass pattern
        // (immediate crossfade, no IntegerDelayLine).
        juce::AudioBuffer<float> dryScratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PanoramaProcessor)
    };
}
