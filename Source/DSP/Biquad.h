#pragma once

#include <cmath>
#include <vector>

/*
    Small allocation-free DSP building blocks for Source/DSP/PreampProcessor.

    Deliberately NOT juce::dsp::IIR::Filter: that class stores coefficients
    behind a heap-allocated, reference-counted Coefficients object, and its
    makeLowPass()/makeHighPass()/... factories allocate a new one every
    call. PreampProcessor recomputes its filter shapes every block (Low
    Cut/High Cut/colour move continuously with PREAMP DRIVE), and doing
    that on the audio thread would violate the project's no-allocation
    rule - so these types hold their coefficients as plain floats that
    setCoefficients() overwrites in place.
*/

namespace uni76::dsp
{
    inline constexpr double twoPi = 6.283185307179586476925286766559;

    /** Direct-Form II Transposed biquad, plain-float coefficients. */
    class Biquad
    {
    public:
        void setCoefficients (float b0In, float b1In, float b2In, float a1In, float a2In) noexcept
        {
            b0 = b0In; b1 = b1In; b2 = b2In; a1 = a1In; a2 = a2In;
        }

        void reset() noexcept { z1 = 0.0f; z2 = 0.0f; }

        float processSample (float x) noexcept
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }

    private:
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
    };

    /** One-pole lowpass, y[n] += alpha * (x[n] - y[n]) - also doubles as the
        "transformer core" transient-rounding/memory stage. */
    class OnePoleLowPass
    {
    public:
        void setCutoffHz (double sampleRate, float cutoffHz) noexcept
        {
            const auto fc = std::max (1.0f, cutoffHz);
            alpha = 1.0f - std::exp ((float) (-twoPi * fc / sampleRate));
        }

        void reset() noexcept { z = 0.0f; }

        float processSample (float x) noexcept
        {
            z += alpha * (x - z);
            return z;
        }

    private:
        float alpha = 1.0f;
        float z = 0.0f;
    };

    /** Classic leaky-integrator DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1]. */
    class DcBlocker
    {
    public:
        void setCutoffHz (double sampleRate, float cutoffHz) noexcept
        {
            // Small-angle approximation, valid since cutoffHz << sampleRate
            // for any DC-blocker use (a few Hz against tens of kHz).
            r = 1.0f - (float) (twoPi * cutoffHz / sampleRate);
        }

        void reset() noexcept { xPrev = 0.0f; yPrev = 0.0f; }

        float processSample (float x) noexcept
        {
            const auto y = x - xPrev + r * yPrev;
            xPrev = x;
            yPrev = y;
            return y;
        }

    private:
        float r = 0.999f;
        float xPrev = 0.0f, yPrev = 0.0f;
    };

    /** Fixed-length integer-sample delay, used to time-align an
        unprocessed ("dry") signal with a path that has real algorithmic
        latency (see PreampProcessor's bypass crossfade), so the plugin's
        declared getLatencySamples() stays correct and constant regardless
        of the module-enabled/bypass state. */
    class IntegerDelayLine
    {
    public:
        void prepare (int delaySamples)
        {
            delay = std::max (0, delaySamples);
            buffer.assign ((size_t) std::max (1, delay), 0.0f);
            writeIndex = 0;
        }

        void reset() noexcept
        {
            std::fill (buffer.begin(), buffer.end(), 0.0f);
            writeIndex = 0;
        }

        float processSample (float x) noexcept
        {
            if (delay == 0)
                return x;

            const auto y = buffer[(size_t) writeIndex];
            buffer[(size_t) writeIndex] = x;
            writeIndex = (writeIndex + 1) % delay;
            return y;
        }

    private:
        std::vector<float> buffer;
        int delay = 0;
        int writeIndex = 0;
    };

    // ---- RBJ Audio EQ Cookbook coefficient generators -----------------
    // Maximally-flat (Q = 1/sqrt(2), no resonance) 2nd-order Butterworth
    // shapes - a soft ~12 dB/oct slope, never a resonant peak.

    inline void makeHighPassButterworth (Biquad& biquad, double sampleRate, float frequencyHz) noexcept
    {
        const auto w0    = twoPi * (double) frequencyHz / sampleRate;
        const auto cosw0 = std::cos (w0);
        const auto sinw0 = std::sin (w0);
        const auto alpha = sinw0 / (2.0 * 0.70710678118654752440);

        const auto a0 = 1.0 + alpha;
        const auto b0 =  (1.0 + cosw0) / 2.0 / a0;
        const auto b1 = -(1.0 + cosw0)       / a0;
        const auto b2 =  (1.0 + cosw0) / 2.0 / a0;
        const auto a1 = -2.0 * cosw0         / a0;
        const auto a2 =  (1.0 - alpha)       / a0;

        biquad.setCoefficients ((float) b0, (float) b1, (float) b2, (float) a1, (float) a2);
    }

    inline void makeLowPassButterworth (Biquad& biquad, double sampleRate, float frequencyHz) noexcept
    {
        const auto w0    = twoPi * (double) frequencyHz / sampleRate;
        const auto cosw0 = std::cos (w0);
        const auto sinw0 = std::sin (w0);
        const auto alpha = sinw0 / (2.0 * 0.70710678118654752440);

        const auto a0 = 1.0 + alpha;
        const auto b0 =  (1.0 - cosw0) / 2.0 / a0;
        const auto b1 =  (1.0 - cosw0)       / a0;
        const auto b2 =  (1.0 - cosw0) / 2.0 / a0;
        const auto a1 = -2.0 * cosw0         / a0;
        const auto a2 =  (1.0 - alpha)       / a0;

        biquad.setCoefficients ((float) b0, (float) b1, (float) b2, (float) a1, (float) a2);
    }

    inline void makeLowShelf (Biquad& biquad, double sampleRate, float frequencyHz, float gainDb, float shelfSlope = 1.0f) noexcept
    {
        const auto A     = std::pow (10.0, (double) gainDb / 40.0);
        const auto w0     = twoPi * (double) frequencyHz / sampleRate;
        const auto cosw0  = std::cos (w0);
        const auto sinw0  = std::sin (w0);
        const auto alpha  = sinw0 / 2.0 * std::sqrt ((A + 1.0 / A) * (1.0 / (double) shelfSlope - 1.0) + 2.0);
        const auto sqrtA2 = 2.0 * std::sqrt (A) * alpha;

        const auto a0 =        (A + 1.0) + (A - 1.0) * cosw0 + sqrtA2;
        const auto b0 =    A * ((A + 1.0) - (A - 1.0) * cosw0 + sqrtA2)     / a0;
        const auto b1 =  2*A * ((A - 1.0) - (A + 1.0) * cosw0)              / a0;
        const auto b2 =    A * ((A + 1.0) - (A - 1.0) * cosw0 - sqrtA2)     / a0;
        const auto a1 =   -2.0 * ((A - 1.0) + (A + 1.0) * cosw0)            / a0;
        const auto a2 =        ((A + 1.0) + (A - 1.0) * cosw0 - sqrtA2)     / a0;

        biquad.setCoefficients ((float) b0, (float) b1, (float) b2, (float) a1, (float) a2);
    }
}
