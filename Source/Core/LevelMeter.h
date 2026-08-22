#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <cmath>

/*
    Lock-free peak-level tracker for driving the INPUT/OUTPUT meter
    telemetry in the UI - not an audio parameter, not saved in state.

    pushBlock() is the only member ever called from the audio thread: it
    computes this block's peak magnitude (no allocation, no locking - see
    juce::AudioBuffer::getMagnitude) and folds it into a single atomic via
    a compare-and-swap "keep the max" loop, so concurrent pushes from the
    audio thread and a read from the message thread can never block each
    other.

    readAndResetPeak() is the only member ever called from the message
    thread: it atomically takes the accumulated peak since the last read
    and resets the counter to zero, ready to accumulate the next reporting
    interval. Envelope smoothing (attack/release) happens in the caller
    (see Source/UI/WebUIEditor.cpp), not here - this class only ever
    reports the raw peak magnitude for whatever block(s) arrived since the
    last read.
*/

namespace uni76
{
    class LevelMeter
    {
    public:
        void pushBlock (const juce::AudioBuffer<float>& buffer) noexcept
        {
            if (buffer.getNumChannels() == 0 || buffer.getNumSamples() == 0)
                return;

            const auto blockPeak = buffer.getMagnitude (0, buffer.getNumSamples());

            // Guard against NaN/Inf getting into the atomic and poisoning
            // every read after it - a corrupt sample should never be able
            // to permanently pin the meter to a nonsense value.
            if (! std::isfinite (blockPeak))
                return;

            auto current = peak.load (std::memory_order_relaxed);

            while (blockPeak > current
                   && ! peak.compare_exchange_weak (current, blockPeak, std::memory_order_relaxed))
            {
                // current is updated with the latest value by compare_exchange_weak on failure;
                // loop again until we either win the swap or another thread already
                // pushed something >= blockPeak.
            }
        }

        /** Message-thread only: returns the peak seen since the last call and resets it to 0. */
        float readAndResetPeak() noexcept
        {
            return peak.exchange (0.0f, std::memory_order_relaxed);
        }

    private:
        std::atomic<float> peak { 0.0f };
    };
}
