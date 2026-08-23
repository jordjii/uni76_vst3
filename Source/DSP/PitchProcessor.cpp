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
        // regardless of the semitone value or enabled state (see
        // getLatencySamples()'s doc comment).
        latencySamples = engine->stretchers[0].inputLatency() + engine->stretchers[0].outputLatency();

        dryScratch.setSize (numChannels, maximumBlockSize, false, false, true);
        wetScratch.setSize (numChannels, maximumBlockSize, false, false, true);
        bypassRampScratch.assign ((size_t) maximumBlockSize, 0.0f);

        bypassSmoother.reset (sampleRate, pitchBypassSmoothingSeconds);
        bypassSmoother.setCurrentAndTargetValue (1.0f);

        for (int ch = 0; ch < maxChannels; ++ch)
            dryDelays[(size_t) ch].prepare (latencySamples);

        reset();
    }

    void PitchProcessor::reset() noexcept
    {
        for (auto& stretcher : engine->stretchers)
            stretcher.reset();

        for (auto& delay : dryDelays)
            delay.reset();
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

        // ---- latency-aligned dry copy for the enable/disable crossfade -
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* dry = dryScratch.getWritePointer (ch);
            const auto* in = buffer.getReadPointer (ch);

            for (int i = 0; i < numSamples; ++i)
                dry[i] = dryDelays[(size_t) ch].processSample (in[i]);
        }

        bypassSmoother.setTargetValue (enabled ? 1.0f : 0.0f);

        const auto clampedSemitones = (float) juce::jlimit (-12, 12, semitones);

        // ---- wet: pure pitch-shift, duration always preserved (equal
        // input/output sample counts every call, so the engine never
        // time-stretches, only pitch-shifts) --------------------------
        for (int ch = 0; ch < channels; ++ch)
        {
            float* inputChannels[1]  = { buffer.getWritePointer (ch) };
            float* outputChannels[1] = { wetScratch.getWritePointer (ch) };

            auto& stretcher = engine->stretchers[(size_t) ch];
            stretcher.setTransposeSemitones (clampedSemitones);
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
