#include "PitchProcessor.h"
#include "PitchCurves.h"

#include <cmath>

// Vendored MIT-licensed library - see ThirdParty/signalsmith-stretch/ and
// docs/DSP_PITCH.md's "Dependency and license" section for the exact
// pinned upstream commit and license text. Vendored verbatim from
// upstream (never hand-edited), so its own warnings are suppressed here
// rather than patched at the source - this project otherwise builds at
// zero warnings.
#if defined (_MSC_VER)
 #pragma warning (push, 0)
#elif defined (__clang__) || defined (__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wall"
 #pragma GCC diagnostic ignored "-Wextra"
 #pragma GCC diagnostic ignored "-Wconversion"
 #pragma GCC diagnostic ignored "-Wshadow"
#endif

#include "../../ThirdParty/signalsmith-stretch/signalsmith-stretch.h"

#if defined (_MSC_VER)
 #pragma warning (pop)
#elif defined (__clang__) || defined (__GNUC__)
 #pragma GCC diagnostic pop
#endif

namespace uni76::dsp
{
    struct PitchProcessor::Engine
    {
        // A single, shared multi-channel engine - see PitchProcessor.h's
        // class comment for why this replaced the previous "two
        // independent mono instances" design. configure()'s first
        // argument is the real channel count (1 for a mono bus, 2 for
        // stereo) - never hardcoded to maxChannels, since the library's
        // own per-band "lock every other channel's phase to the
        // highest-energy one" step is keyed off this exact count.
        signalsmith::stretch::SignalsmithStretch<float> stretcher;
    };

    PitchProcessor::PitchProcessor() : engine (std::make_unique<Engine>()) {}
    PitchProcessor::~PitchProcessor() = default;

    void PitchProcessor::prepare (double sampleRateIn, int maximumBlockSize, int numChannelsToUse)
    {
        sampleRate = sampleRateIn;
        numChannels = juce::jlimit (1, maxChannels, numChannelsToUse);

        // See docs/DSP_PITCH.md's "Configuration benchmark" section for
        // why this specific block/interval pair (measured clearly more
        // stable low-frequency tracking than the library's own
        // presetDefault()) was chosen over letting the block size scale
        // with the host's buffer size.
        const auto blockSamples    = juce::jmax (4, juce::roundToInt (sampleRate * pitchStftBlockSeconds));
        const auto intervalSamples = juce::jmax (1, juce::roundToInt (sampleRate * pitchStftIntervalSeconds));

        engine->stretcher.configure (numChannels, blockSamples, intervalSamples);

        // A single instance now, so latency is read from it directly -
        // the previous "both channels share one configuration, take
        // channel 0's figure" comment no longer applies, there is only
        // one configuration. This is the engine's own latency *while
        // running*, not the module's current real output delay (see
        // getLatencySamples()'s doc comment).
        latencySamples = engine->stretcher.inputLatency() + engine->stretcher.outputLatency();

        dryScratch.setSize (numChannels, maximumBlockSize, false, false, true);
        wetScratch.setSize (numChannels, maximumBlockSize, false, false, true);
        bypassRampScratch.assign ((size_t) maximumBlockSize, 0.0f);

        bypassSmoother.reset (sampleRate, pitchBypassSmoothingSeconds);
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        reset();
    }

    void PitchProcessor::reset() noexcept
    {
        engine->stretcher.reset();
    }

    void PitchProcessor::process (juce::AudioBuffer<float>& buffer, int semitones, bool enabled) noexcept
    {
        const auto numSamples = buffer.getNumSamples();

        if (numSamples <= 0)
            return;

        // The shared engine was configured for exactly `numChannels`
        // channels (see prepare()) - its per-band "lock every other
        // channel's phase to the highest-energy one" logic is written in
        // terms of that fixed count, so it cannot safely be fed a
        // different channel count block-to-block. A host's channel count
        // is fixed for the lifetime of a prepareToPlay() call in
        // practice (this is the same assumption every other module here
        // already makes), so this is a defensive bound, not an expected
        // runtime path.
        const auto channels = buffer.getNumChannels();
        if (channels < numChannels)
            return;

        // ---- numeric safety at the input boundary --------------------
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                if (! std::isfinite (data[i]))
                    data[i] = 0.0f;
        }

        // ---- true bypass fast path -------------------------------------
        // Settled disabled (target already reached, and staying disabled
        // this block too) - skip the engine entirely: no delay, no CPU
        // cost, genuinely zero added latency. This is the common resting
        // state (every factory preset leaves PITCH disabled) and the
        // whole point of this design - see the class comment's
        // "Enable/disable behaviour" section.
        const auto targetMix = enabled ? 1.0f : 0.0f;
        const auto settled = std::abs (bypassSmoother.getCurrentValue() - targetMix) < 1.0e-4f;

        if (! enabled && settled)
            return;

        bypassSmoother.setTargetValue (targetMix);

        // ---- live (undelayed) dry copy for the enable/disable blend ----
        // Not a delay-aligned crossfade any more - see the class comment
        // for why that is no longer possible once the disabled path has
        // zero latency of its own. Captured before the engine call below,
        // same ordering the previous delay-line version used.
        for (int ch = 0; ch < numChannels; ++ch)
            dryScratch.copyFrom (ch, 0, buffer, ch, 0, numSamples);

        const auto clampedSemitones = (float) juce::jlimit (-12, 12, semitones);
        engine->stretcher.setTransposeSemitones (clampedSemitones, (float) (pitchTonalityLimitHz / sampleRate));

        // ---- wet: pure pitch-shift, duration always preserved (equal
        // input/output sample counts every call, so the engine never
        // time-stretches, only pitch-shifts) --------------------------
        // One call across all channels, not a per-channel loop - this is
        // what gives the engine's own STFT grid and phase-locking a
        // shared view of every channel at once (see PitchProcessor.h's
        // class comment).
        float* inputChannels[maxChannels];
        float* outputChannels[maxChannels];
        for (int ch = 0; ch < numChannels; ++ch)
        {
            inputChannels[ch]  = buffer.getWritePointer (ch);
            outputChannels[ch] = wetScratch.getWritePointer (ch);
        }
        engine->stretcher.process (inputChannels, numSamples, outputChannels, numSamples);

        // ---- enable/disable crossfade (sample-accurate) ----------------
        for (int i = 0; i < numSamples; ++i)
            bypassRampScratch[(size_t) i] = bypassSmoother.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* out = buffer.getWritePointer (ch);
            const auto* wet = wetScratch.getReadPointer (ch);
            const auto* dry = dryScratch.getReadPointer (ch);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto mix = bypassRampScratch[(size_t) i];
                out[i] = dry[i] * (1.0f - mix) + wet[i] * mix;
            }
        }

        // ---- final numeric safety net ------------------------------------
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                if (! std::isfinite (data[i]))
                    data[i] = 0.0f;
        }
    }
}
