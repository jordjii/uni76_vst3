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
        // Two fully independent mono engines, not one shared 2-channel
        // engine - see the class comment in PitchProcessor.h for why.
        std::array<signalsmith::stretch::SignalsmithStretch<float>, PitchProcessor::maxChannels> stretchers;
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

        for (auto& stretcher : engine->stretchers)
            stretcher.configure (1, blockSamples, intervalSamples);

        // Both channels share the same configuration, so their latency is
        // identical - a single scalar is correct here, and stays fixed
        // regardless of the semitone value. This is the engine's own
        // latency *while running*, not the module's current real output
        // delay (see getLatencySamples()'s doc comment).
        latencySamples = engine->stretchers[0].inputLatency() + engine->stretchers[0].outputLatency();

        dryScratch.setSize (numChannels, maximumBlockSize, false, false, true);
        wetScratch.setSize (numChannels, maximumBlockSize, false, false, true);
        bypassRampScratch.assign ((size_t) maximumBlockSize, 0.0f);

        bypassSmoother.reset (sampleRate, pitchBypassSmoothingSeconds);
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        reset();
    }

    void PitchProcessor::reset() noexcept
    {
        for (auto& stretcher : engine->stretchers)
            stretcher.reset();
    }

    void PitchProcessor::process (juce::AudioBuffer<float>& buffer, int semitones, bool enabled) noexcept
    {
        const auto numSamples = buffer.getNumSamples();
        const auto channels   = juce::jmin (numChannels, buffer.getNumChannels());

        if (numSamples <= 0 || channels <= 0)
            return;

        // ---- numeric safety at the input boundary --------------------
        for (int ch = 0; ch < channels; ++ch)
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
        for (int ch = 0; ch < channels; ++ch)
            dryScratch.copyFrom (ch, 0, buffer, ch, 0, numSamples);

        const auto clampedSemitones = (float) juce::jlimit (-12, 12, semitones);

        // ---- wet: pure pitch-shift, duration always preserved (equal
        // input/output sample counts every call, so the engine never
        // time-stretches, only pitch-shifts) --------------------------
        for (int ch = 0; ch < channels; ++ch)
        {
            float* inputChannels[1]  = { buffer.getWritePointer (ch) };
            float* outputChannels[1] = { wetScratch.getWritePointer (ch) };

            auto& stretcher = engine->stretchers[(size_t) ch];
            // Tonality limit (see PitchCurves.h's pitchTonalityLimitHz) -
            // the API takes it normalised against sample rate, not a raw
            // Hz value (see the library's own setTransposeFactor()
            // comment / UPSTREAM_README.md).
            stretcher.setTransposeSemitones (clampedSemitones, (float) (pitchTonalityLimitHz / sampleRate));
            stretcher.process (inputChannels, numSamples, outputChannels, numSamples);
        }

        // ---- enable/disable crossfade (sample-accurate) ----------------
        for (int i = 0; i < numSamples; ++i)
            bypassRampScratch[(size_t) i] = bypassSmoother.getNextValue();

        for (int ch = 0; ch < channels; ++ch)
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
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
                if (! std::isfinite (data[i]))
                    data[i] = 0.0f;
        }
    }
}
