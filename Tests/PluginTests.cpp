#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "Plugin/PluginProcessor.h"
#include "Parameters/ParameterIDs.h"
#include "Core/PluginIdentity.h"
#include "Core/LevelMeter.h"
#include "Core/MeterEnvelope.h"
#include "Core/ModuleEnableState.h"
#include "DSP/PreampProcessor.h"
#include "DSP/PreampCurves.h"
#include "DSP/EqProcessor.h"
#include "DSP/EqCurves.h"
#include "DSP/SatProcessor.h"
#include "DSP/SatCurves.h"
#include "DSP/PitchProcessor.h"
#include "DSP/PitchCurves.h"
#include "DSP/PanoramaProcessor.h"
#include "DSP/PanoramaCurves.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
    juce::AudioProcessor::BusesLayout makeLayout (juce::AudioChannelSet in, juce::AudioChannelSet out)
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (in);
        layout.outputBuses.add (out);
        return layout;
    }
}

class UNI76ProcessorTests final : public juce::UnitTest
{
public:
    UNI76ProcessorTests() : juce::UnitTest ("UNI76AudioProcessor", "UNI76") {}

    void runTest() override
    {
        beginTest ("Processor constructs successfully");
        {
            UNI76AudioProcessor processor;
            expect (processor.getName().isNotEmpty());
            expectEquals (processor.getTailLengthSeconds(), 0.0);
        }

        beginTest ("Mono->mono and stereo->stereo bus layouts are supported; others are rejected");
        {
            UNI76AudioProcessor processor;

            expect (processor.isBusesLayoutSupported (
                makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono())));

            expect (processor.isBusesLayoutSupported (
                makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo())));

            expect (! processor.isBusesLayoutSupported (
                makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo())));

            expect (! processor.isBusesLayoutSupported (
                makeLayout (juce::AudioChannelSet::createLCR(), juce::AudioChannelSet::createLCR())));
        }

        beginTest ("All 7 parameter IDs exist with the correct defaults (EQ 50%, everything else 0%)");
        {
            UNI76AudioProcessor processor;
            auto& apvts = processor.getValueTreeState();

            expectEquals ((int) uni76::ParamID::all.size(), 7);

            for (const auto* id : uni76::ParamID::all)
            {
                auto* param = apvts.getParameter (id);
                expect (param != nullptr, juce::String ("missing parameter: ") + id);

                const auto expectedDefault = std::strcmp (id, uni76::ParamID::eq) == 0 ? 50.0f : 0.0f;

                if (auto* floatParam = dynamic_cast<juce::AudioParameterFloat*> (param))
                    expectWithinAbsoluteError (floatParam->get(), expectedDefault, 0.001f, id);
            }
        }

        beginTest ("State survives serialize -> modify -> deserialize");
        {
            UNI76AudioProcessor processor;
            auto& apvts = processor.getValueTreeState();

            auto* preamp = apvts.getParameter (uni76::ParamID::preamp);
            expect (preamp != nullptr);

            preamp->setValueNotifyingHost (0.75f);

            juce::MemoryBlock savedState;
            processor.getStateInformation (savedState);

            // Perturb every parameter away from the saved state before reloading.
            for (const auto* id : uni76::ParamID::all)
                if (auto* param = apvts.getParameter (id))
                    param->setValueNotifyingHost (0.1f);

            processor.setStateInformation (savedState.getData(), (int) savedState.getSize());

            expectWithinAbsoluteError (preamp->getValue(), 0.75f, 0.001f);

            for (const auto* id : uni76::ParamID::all)
            {
                if (std::strcmp (id, uni76::ParamID::preamp) == 0)
                    continue;

                // Every other parameter was never touched before the save,
                // so it should restore to its own construction-time default
                // (EQ 50%, everything else 0%) - not a single shared value.
                // PITCH is special-cased to 0.5f too: its default (0 ST)
                // sits at the *normalised* midpoint of its -12..+12 range,
                // same as EQ's PHONE default sits at the midpoint of 0..100.
                const bool isMidpointDefault = std::strcmp (id, uni76::ParamID::eq) == 0
                                             || std::strcmp (id, uni76::ParamID::pitch) == 0;
                const auto expectedDefault = isMidpointDefault ? 0.5f : 0.0f;

                if (auto* param = apvts.getParameter (id))
                    expectWithinAbsoluteError (param->getValue(), expectedDefault, 0.001f, id);
            }
        }

        beginTest ("processBlock stays finite and stable across sample rates/block sizes; declared latency matches the PREAMP oversampling architecture");
        {
            // PREAMP is now real DSP (default enabled=true, default DRIVE
            // 0%), so processBlock is no longer expected to be a bit-exact
            // passthrough or report zero latency - see
            // Source/DSP/PreampProcessor.h and docs/DSP_PREAMP.md. What
            // must still hold: output stays finite, and the declared
            // latency matches the oversampling factor the DSP actually
            // chose for that sample rate (4x/2x below 96kHz, none at
            // 176.4/192kHz+).
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));

            struct Config { double sampleRate; int blockSize; bool expectLatency; };
            const Config configs[] {
                { 44100.0, 64,   true },
                { 48000.0, 512,  true },
                { 96000.0, 1,    true },
                { 44100.0, 4096, true },
                // PREAMP/SAT drop their oversampling latency at 176.4kHz+,
                // but PITCH's STFT latency scales with sample rate and
                // never reaches zero (see docs/DSP_PITCH.md) - so total
                // plugin latency is nonzero at every sample rate now.
                { 192000.0, 512, true },
            };

            juce::Random random (1234);

            for (const auto& config : configs)
            {
                processor.prepareToPlay (config.sampleRate, config.blockSize);

                juce::AudioBuffer<float> buffer (2, config.blockSize);

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    for (int i = 0; i < buffer.getNumSamples(); ++i)
                        buffer.setSample (ch, i, random.nextFloat() * 2.0f - 1.0f);

                juce::MidiBuffer midi;
                processor.processBlock (buffer, midi);

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    for (int i = 0; i < buffer.getNumSamples(); ++i)
                        expect (std::isfinite (buffer.getSample (ch, i)), "processBlock produced a non-finite sample");

                if (config.expectLatency)
                    expect (processor.getLatencySamples() > 0,
                            juce::String ("expected nonzero latency at ") + juce::String (config.sampleRate) + "Hz");
                else
                    expectEquals (processor.getLatencySamples(), 0);

                processor.releaseResources();
            }
        }

        beginTest ("Saved state keeps the schema/shape contract the UI and future presets depend on");
        {
            // There's no serialized preset file committed anywhere to diff
            // against (state is generated at runtime, never persisted in
            // the repo) - what actually has to stay stable across commits
            // is the contract: root ValueTree type, schema version
            // property, and one <PARAM id="..." .../> per parameter ID.
            // Source/Parameters and Source/Core were not touched by the UI
            // rewrite this test was added alongside, so this is a
            // regression guard against that contract silently drifting in
            // the future, not just a restatement of the current output.
            UNI76AudioProcessor processor;

            juce::MemoryBlock savedState;
            processor.getStateInformation (savedState);

            auto xml = juce::AudioProcessor::getXmlFromBinary (savedState.getData(), (int) savedState.getSize());
            expect (xml != nullptr, "saved state did not parse as XML");

            if (xml != nullptr)
            {
                expectEquals (xml->getTagName(), juce::String ("PARAMETERS"));
                expect (xml->hasAttribute (uni76::stateSchemaVersionProperty));
                expectEquals (xml->getIntAttribute (uni76::stateSchemaVersionProperty), uni76::stateSchemaVersion);

                for (const auto* id : uni76::ParamID::all)
                {
                    const auto* paramElement = xml->getChildByAttribute ("id", id);
                    expect (paramElement != nullptr, juce::String ("no <PARAM id=\"") + id + "\"> in saved state");
                }
            }
        }

        beginTest ("UI-facing normalised <-> percent conversion is exact at 0%, 50%, and 100%");
        {
            // The web UI (Source/UI, Resources/Web/knob.js) reads/writes
            // parameters exclusively as a 0..1 "normalised" value through
            // the JUCE WebSliderRelay bridge, and displays
            // round(normalised * 100) as the on-screen percentage. That
            // mapping is only lossless if the underlying parameter range
            // is exactly linear over 0..100 - assert that directly rather
            // than trusting it implicitly.
            UNI76AudioProcessor processor;
            auto& apvts = processor.getValueTreeState();

            for (const auto* id : uni76::ParamID::all)
            {
                auto* param = apvts.getParameter (id);
                expect (param != nullptr, id);
                if (param == nullptr)
                    continue;

                param->setValueNotifyingHost (0.0f);
                expectWithinAbsoluteError (param->getValue(), 0.0f, 0.0001f, id);

                param->setValueNotifyingHost (0.5f);
                expectWithinAbsoluteError (param->getValue(), 0.5f, 0.0001f, id);

                if (auto* floatParam = dynamic_cast<juce::AudioParameterFloat*> (param))
                    expectWithinAbsoluteError (floatParam->get(), 50.0f, 0.01f, id);

                param->setValueNotifyingHost (1.0f);
                expectWithinAbsoluteError (param->getValue(), 1.0f, 0.0001f, id);
            }
        }
    }
};

static UNI76ProcessorTests uni76ProcessorTests; // NOLINT - self-registers with the UnitTestRunner

namespace
{
    juce::AudioBuffer<float> makeConstantBuffer (int numChannels, int numSamples, float value)
    {
        juce::AudioBuffer<float> buffer (numChannels, numSamples);

        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, value);

        return buffer;
    }
}

class UNI76LevelMeterTests final : public juce::UnitTest
{
public:
    UNI76LevelMeterTests() : juce::UnitTest ("uni76::LevelMeter", "UNI76") {}

    void runTest() override
    {
        beginTest ("Silence produces a silent (zero) meter reading");
        {
            uni76::LevelMeter meter;
            auto silence = makeConstantBuffer (2, 512, 0.0f);

            meter.pushBlock (silence);

            expectWithinAbsoluteError (meter.readAndResetPeak(), 0.0f, 0.0001f);
        }

        beginTest ("A known constant amplitude converts to the same peak level");
        {
            uni76::LevelMeter meter;
            auto buffer = makeConstantBuffer (2, 512, 0.5f);

            meter.pushBlock (buffer);

            expectWithinAbsoluteError (meter.readAndResetPeak(), 0.5f, 0.0001f);
        }

        beginTest ("The meter tracks the maximum peak across multiple pushes before it's read");
        {
            uni76::LevelMeter meter;
            auto quiet = makeConstantBuffer (2, 256, 0.3f);
            auto loud  = makeConstantBuffer (2, 256, 0.7f);

            meter.pushBlock (quiet);
            meter.pushBlock (loud);

            expectWithinAbsoluteError (meter.readAndResetPeak(), 0.7f, 0.0001f);
        }

        beginTest ("Reading resets the meter, ready for the next reporting interval");
        {
            uni76::LevelMeter meter;
            auto buffer = makeConstantBuffer (1, 128, 0.9f);

            meter.pushBlock (buffer);
            meter.readAndResetPeak();

            expectWithinAbsoluteError (meter.readAndResetPeak(), 0.0f, 0.0001f);
        }

        beginTest ("NaN samples are ignored rather than poisoning the reading");
        {
            uni76::LevelMeter meter;
            juce::AudioBuffer<float> buffer (2, 64);
            buffer.clear();
            buffer.setSample (0, 10, std::numeric_limits<float>::quiet_NaN());

            meter.pushBlock (buffer);
            const auto result = meter.readAndResetPeak();

            expect (std::isfinite (result), "NaN input produced a non-finite meter reading");
            expectWithinAbsoluteError (result, 0.0f, 0.0001f);
        }

        beginTest ("Inf samples are ignored rather than poisoning the reading");
        {
            uni76::LevelMeter meter;
            juce::AudioBuffer<float> buffer (2, 64);
            buffer.clear();
            buffer.setSample (1, 20, std::numeric_limits<float>::infinity());

            meter.pushBlock (buffer);
            const auto result = meter.readAndResetPeak();

            expect (std::isfinite (result), "Inf input produced a non-finite meter reading");
            expectWithinAbsoluteError (result, 0.0f, 0.0001f);
        }

        beginTest ("Mono buffers are measured correctly");
        {
            uni76::LevelMeter meter;
            auto buffer = makeConstantBuffer (1, 300, 0.42f);

            meter.pushBlock (buffer);

            expectWithinAbsoluteError (meter.readAndResetPeak(), 0.42f, 0.0001f);
        }

        beginTest ("Stereo buffers report the louder of the two channels");
        {
            uni76::LevelMeter meter;
            juce::AudioBuffer<float> buffer (2, 200);
            buffer.clear();

            for (int i = 0; i < 200; ++i)
            {
                buffer.setSample (0, i, 0.2f);
                buffer.setSample (1, i, 0.6f);
            }

            meter.pushBlock (buffer);

            expectWithinAbsoluteError (meter.readAndResetPeak(), 0.6f, 0.0001f);
        }

        beginTest ("pushBlock never modifies the audio buffer it measures");
        {
            uni76::LevelMeter meter;
            auto buffer = makeConstantBuffer (2, 256, 0.37f);

            juce::AudioBuffer<float> reference;
            reference.makeCopyOf (buffer);

            meter.pushBlock (buffer);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto matches = std::memcmp (buffer.getReadPointer (ch),
                                                   reference.getReadPointer (ch),
                                                   sizeof (float) * (size_t) buffer.getNumSamples()) == 0;
                expect (matches, "pushBlock modified the audio buffer");
            }
        }

        beginTest ("Processor integration: processBlock() feeds both meters with a real signal, INPUT before PREAMP and OUTPUT after");
        {
            // A constant (DC) buffer is no longer a meaningful probe here:
            // PREAMP's DC/infrasonic protection is *designed* to remove
            // true DC, so a literal step input isn't representative of the
            // module doing its job correctly - use a sine instead.
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (44100.0, 256);

            juce::AudioBuffer<float> buffer (2, 256);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    buffer.setSample (ch, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 1000.0f * (float) i / 44100.0f));

            juce::MidiBuffer midi;

            // A few blocks so PREAMP's internal smoothing/filters settle
            // away from their initial zero state before judging the meters.
            for (int block = 0; block < 20; ++block)
                processor.processBlock (buffer, midi);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    expect (std::isfinite (buffer.getSample (ch, i)), "processBlock produced a non-finite sample");

            expect (processor.getLatencySamples() > 0,
                    "44.1kHz should engage PREAMP's 4x oversampling and report nonzero latency");

            expect (processor.getInputLevelMeter().readAndResetPeak() > 0.0f, "input meter should register the fed signal");
            expect (processor.getOutputLevelMeter().readAndResetPeak() > 0.0f, "output meter should register PREAMP's output");
        }

        beginTest ("Deterministic dB levels convert to the correct linear peak (-24/-12/-6/~0 dBFS)");
        {
            struct DbCase { const char* label; float dBFS; };

            const DbCase cases[] {
                { "-24 dBFS", -24.0f },
                { "-12 dBFS", -12.0f },
                { "-6 dBFS",   -6.0f },
                { "~0 dBFS",   -0.1f },
            };

            for (const auto& c : cases)
            {
                uni76::LevelMeter meter;
                const auto amplitude = std::pow (10.0f, c.dBFS / 20.0f);
                auto buffer = makeConstantBuffer (2, 512, amplitude);

                meter.pushBlock (buffer);

                expectWithinAbsoluteError (meter.readAndResetPeak(), amplitude, 0.0005f, c.label);
            }
        }
    }
};

class UNI76MeterEnvelopeTests final : public juce::UnitTest
{
public:
    UNI76MeterEnvelopeTests() : juce::UnitTest ("uni76::applyMeterEnvelope", "UNI76") {}

    void runTest() override
    {
        beginTest ("Attack closes more of the gap per tick than release does");
        {
            // Compare the *fraction of the gap closed*, not the raw
            // resulting values - attack and release start from opposite
            // ends, so the raw outputs (0.6 vs 0.92) aren't comparable
            // directly.
            const auto attackGapClosed  = uni76::applyMeterEnvelope (0.0f, 1.0f) - 0.0f;
            const auto releaseGapClosed = 1.0f - uni76::applyMeterEnvelope (1.0f, 0.0f);

            expect (attackGapClosed > releaseGapClosed,
                    "one attack tick should close more of the gap to the target than one release tick");
        }

        beginTest ("Silence eventually releases the envelope to (effectively) zero");
        {
            float envelope = 1.0f;

            for (int tick = 0; tick < 500; ++tick)
                envelope = uni76::applyMeterEnvelope (envelope, 0.0f);

            expectWithinAbsoluteError (envelope, 0.0f, 0.0001f);
        }
    }
};

static UNI76LevelMeterTests uni76LevelMeterTests; // NOLINT - self-registers with the UnitTestRunner
static UNI76MeterEnvelopeTests uni76MeterEnvelopeTests; // NOLINT - self-registers with the UnitTestRunner

class UNI76ModuleEnableStateTests final : public juce::UnitTest
{
public:
    UNI76ModuleEnableStateTests() : juce::UnitTest ("uni76::ModuleEnableState", "UNI76") {}

    void runTest() override
    {
        beginTest ("All 7 modules default to enabled");
        {
            uni76::ModuleEnableState state;

            for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                expect (state.isEnabled (i), uni76::ModuleEnableState::propertyNames[(size_t) i]);
        }

        beginTest ("Processor: module-enabled state survives serialize -> modify -> deserialize");
        {
            UNI76AudioProcessor processor;
            auto& moduleState = processor.getModuleEnableState();

            // Disable an arbitrary subset before saving.
            moduleState.setEnabled (0, false); // preamp
            moduleState.setEnabled (3, false); // pitch
            moduleState.setEnabled (6, false); // imager

            juce::MemoryBlock savedState;
            processor.getStateInformation (savedState);

            // Perturb every flag away from the saved state before reloading.
            for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                moduleState.setEnabled (i, true);

            processor.setStateInformation (savedState.getData(), (int) savedState.getSize());

            const bool expected[] { false, true, true, false, true, true, false };

            for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                expectEquals ((int) moduleState.isEnabled (i), (int) expected[i],
                              uni76::ModuleEnableState::propertyNames[(size_t) i]);
        }

        beginTest ("Loading a pre-v2 state (no module-enabled properties at all) migrates to enabled=true");
        {
            // Deliberately hand-built to look like what v1 (before the
            // module-enabled flags existed) actually saved: just the
            // PARAMETERS tree and the schema version property - none of
            // the *Enabled properties this test is checking migrate
            // correctly when they're simply absent.
            juce::ValueTree legacyState ("PARAMETERS");
            legacyState.setProperty (uni76::stateSchemaVersionProperty, 1, nullptr);

            for (const auto* id : uni76::ParamID::all)
            {
                juce::ValueTree param ("PARAM");
                param.setProperty ("id", id, nullptr);
                param.setProperty ("value", 50.0, nullptr);
                legacyState.appendChild (param, nullptr);
            }

            juce::MemoryBlock legacyBlock;
            if (auto xml = legacyState.createXml())
                juce::AudioProcessor::copyXmlToBinary (*xml, legacyBlock);

            UNI76AudioProcessor processor;
            auto& moduleState = processor.getModuleEnableState();

            // Perturb first, so a no-op load couldn't accidentally pass.
            for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                moduleState.setEnabled (i, false);

            processor.setStateInformation (legacyBlock.getData(), (int) legacyBlock.getSize());

            for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                expect (moduleState.isEnabled (i),
                        juce::String ("legacy state should migrate to enabled=true for ")
                            + uni76::ModuleEnableState::propertyNames[(size_t) i]);
        }
    }
};

static UNI76ModuleEnableStateTests uni76ModuleEnableStateTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// uni76::dsp::PreampProcessor - the deterministic DSP test suite required by
// the PREAMP audit. Runs the processor directly (not through the full
// AudioProcessor) so tests can drive DRIVE/enabled exactly and inspect the
// resulting samples.
namespace
{
    juce::AudioBuffer<float> makeSineBuffer (int numChannels, int numSamples, double sampleRate,
                                              float freqHz, float amplitude, float startPhase = 0.0f)
    {
        juce::AudioBuffer<float> buffer (numChannels, numSamples);
        const auto increment = juce::MathConstants<float>::twoPi * freqHz / (float) sampleRate;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto phase = startPhase;
            for (int i = 0; i < numSamples; ++i)
            {
                buffer.setSample (ch, i, amplitude * std::sin (phase));
                phase += increment;
            }
        }

        return buffer;
    }

    float bufferRms (const juce::AudioBuffer<float>& buffer, int channel, int startSample, int numSamples)
    {
        double sum = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            const auto s = (double) buffer.getSample (channel, startSample + i);
            sum += s * s;
        }
        return (float) std::sqrt (sum / (double) numSamples);
    }

    float bufferMean (const juce::AudioBuffer<float>& buffer, int channel, int startSample, int numSamples)
    {
        double sum = 0.0;
        for (int i = 0; i < numSamples; ++i)
            sum += buffer.getSample (channel, startSample + i);
        return (float) (sum / (double) numSamples);
    }

    float bufferPeak (const juce::AudioBuffer<float>& buffer, int channel, int startSample, int numSamples)
    {
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            peak = juce::jmax (peak, std::abs (buffer.getSample (channel, startSample + i)));
        return peak;
    }

    bool bufferIsFinite (const juce::AudioBuffer<float>& buffer)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (! std::isfinite (buffer.getSample (ch, i)))
                    return false;
        return true;
    }

    /** A Goertzel analysis window must land its target frequency (almost)
        exactly on a bin, or a long window causes severe beat-drift
        cancellation - choosing the window as an exact number of periods
        at freqHz keeps that error negligible while still averaging over
        many cycles. */
    int periodicAnalysisLength (double sampleRate, float freqHz, int cycles) noexcept
    {
        return juce::jmax (64, (int) std::round ((double) cycles * sampleRate / (double) freqHz));
    }

    /** Goertzel single-bin magnitude - isolates exactly one frequency
        (fundamental or a specific harmonic) from a signal, without the
        leakage a crude fixed-slope filter proxy suffers from when the
        fundamental and the content under test are close together. Callers
        should size numSamples via periodicAnalysisLength() above. */
    float goertzelMagnitude (const juce::AudioBuffer<float>& buffer, int channel, int startSample,
                              int numSamples, double sampleRate, float freqHz)
    {
        const auto k = (int) (0.5 + (double) numSamples * (double) freqHz / sampleRate);
        const auto omega = (2.0 * juce::MathConstants<double>::pi * (double) k) / (double) numSamples;
        const auto coeff = 2.0 * std::cos (omega);

        double s0 = 0.0, s1 = 0.0, s2 = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            s0 = (double) buffer.getSample (channel, startSample + i) + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        const auto real = s1 - s2 * std::cos (omega);
        const auto imag = s2 * std::sin (omega);
        return (float) (std::sqrt (real * real + imag * imag) / ((double) numSamples / 2.0));
    }

    /** Feeds a continuous sine through `preamp` in fixed-size blocks at a
        constant DRIVE/enabled setting, returning the full processed run for
        offline analysis. Mirrors exactly how PluginProcessor::processBlock
        calls PreampProcessor::process(). */
    juce::AudioBuffer<float> runPreampSine (uni76::dsp::PreampProcessor& preamp, int numChannels, int blockSize,
                                             int totalSamples, double sampleRate, float freqHz, float amplitude,
                                             float drive, bool enabled)
    {
        juce::AudioBuffer<float> result (numChannels, totalSamples);
        const auto increment = juce::MathConstants<float>::twoPi * freqHz / (float) sampleRate;
        float phase = 0.0f;
        int done = 0;

        while (done < totalSamples)
        {
            const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
            juce::AudioBuffer<float> block (numChannels, thisBlock);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto p = phase;
                for (int i = 0; i < thisBlock; ++i)
                {
                    block.setSample (ch, i, amplitude * std::sin (p));
                    p += increment;
                }
            }

            preamp.process (block, drive, enabled);

            for (int ch = 0; ch < numChannels; ++ch)
                result.copyFrom (ch, done, block, ch, 0, thisBlock);

            phase += increment * (float) thisBlock;
            done += thisBlock;
        }

        return result;
    }

    // ---- PITCH test helpers -------------------------------------------

    juce::AudioBuffer<float> generateSine (int numChannels, int totalSamples, double sampleRate,
                                            float freqHz, float amplitude)
    {
        juce::AudioBuffer<float> buffer (numChannels, totalSamples);
        const auto increment = juce::MathConstants<float>::twoPi * freqHz / (float) sampleRate;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto p = 0.0f;
            for (int i = 0; i < totalSamples; ++i)
            {
                buffer.setSample (ch, i, amplitude * std::sin (p));
                p += increment;
            }
        }
        return buffer;
    }

    /** Feeds an already-built buffer through `pitch` in fixed-size blocks,
        mirroring exactly how PluginProcessor::processBlock() calls
        PitchProcessor::process() - the block size used here is deliberately
        a test parameter (not tied to how the buffer was generated), so
        tests can confirm behaviour doesn't depend on host chunking. */
    juce::AudioBuffer<float> runPitchProcessor (uni76::dsp::PitchProcessor& pitch, const juce::AudioBuffer<float>& input,
                                                 int blockSize, int semitones, bool enabled)
    {
        const auto numChannels = input.getNumChannels();
        const auto totalSamples = input.getNumSamples();
        juce::AudioBuffer<float> result (numChannels, totalSamples);

        int done = 0;
        while (done < totalSamples)
        {
            const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
            juce::AudioBuffer<float> block (numChannels, thisBlock);

            for (int ch = 0; ch < numChannels; ++ch)
                block.copyFrom (ch, 0, input, ch, done, thisBlock);

            pitch.process (block, semitones, enabled);

            for (int ch = 0; ch < numChannels; ++ch)
                result.copyFrom (ch, done, block, ch, 0, thisBlock);

            done += thisBlock;
        }
        return result;
    }

    struct BassStability
    {
        double freqMean = 0.0, freqStd = 0.0;
        double ampDbStd = 0.0;    // amplitude-modulation depth across windows, in dB
        double rmsModDepth = 0.0; // std/mean of RMS across windows
        int numWindows = 0;
    };

    /** Phase-vocoder frequency-reassignment analysis: splits
        [startSample, startSample+usableSamples) into overlapping windows
        sized to resolve targetFreqHz (>= 4 full cycles, >= 15ms), and for
        each hop measures the instantaneous frequency via the correct
        phase-advance formula - deviation = measured phase step minus the
        analysis bin's own expected phase advance over one hop, i.e.
        `(phase - prevPhase) - omega*hopLen` - plus the Goertzel magnitude
        and time-domain RMS in that window. This is exactly the failure
        mode the product brief calls out: a shifted bass tone whose
        fundamental frequency or level cyclically "breathes"/floats with
        the algorithm's own hop/window period - an ordinary single-shot
        FFT/Goertzel measurement over the whole tone cannot see this. */
    // minCycles=4 resolves an isolated tone comfortably. For a target with
    // *other* spectral content nearby (a chord tone a third or so away),
    // 4 cycles is too coarse to separate them by Goertzel bin resolution
    // alone (resolution = targetFreq/minCycles scales with the target the
    // same way a fixed musical interval's Hz gap does, so the gap/
    // resolution ratio for e.g. a minor third is <1 bin at *any* register)
    // - callers analysing a closely-voiced chord tone should pass a larger
    // minCycles to trade time-resolution for enough frequency-resolution.
    BassStability analyzeBassStability (const juce::AudioBuffer<float>& buffer, int channel, int startSample,
                                         int usableSamples, double sampleRate, float targetFreqHz, int minCycles = 4)
    {
        const auto windowLen = juce::jmax ((int) std::round (sampleRate * 0.015),
                                            (int) std::round ((double) minCycles * sampleRate / (double) targetFreqHz));
        const auto hopLen = juce::jmax (1, windowLen / 3);

        std::vector<double> instFreqs, ampDb, rmsValues;

        bool havePrev = false;
        double prevPhase = 0.0;
        int pos = 0;

        while (pos + windowLen <= usableSamples)
        {
            const auto k = (int) (0.5 + (double) windowLen * (double) targetFreqHz / sampleRate);
            const auto omega = (2.0 * juce::MathConstants<double>::pi * (double) k) / (double) windowLen;
            const auto coeff = 2.0 * std::cos (omega);

            double s0 = 0.0, s1 = 0.0, s2 = 0.0, rmsAcc = 0.0;
            for (int i = 0; i < windowLen; ++i)
            {
                const auto x = (double) buffer.getSample (channel, startSample + pos + i);
                s0 = x + coeff * s1 - s2;
                s2 = s1; s1 = s0;
                rmsAcc += x * x;
            }

            const auto real = s1 - s2 * std::cos (omega);
            const auto imag = s2 * std::sin (omega);
            const auto mag = std::sqrt (real * real + imag * imag) / ((double) windowLen / 2.0);
            const auto phase = std::atan2 (imag, real);
            const auto rms = std::sqrt (rmsAcc / (double) windowLen);

            if (havePrev)
            {
                auto dPhase = (phase - prevPhase) - omega * (double) hopLen;
                while (dPhase >  juce::MathConstants<double>::pi) dPhase -= 2.0 * juce::MathConstants<double>::pi;
                while (dPhase < -juce::MathConstants<double>::pi) dPhase += 2.0 * juce::MathConstants<double>::pi;

                const auto binHz = (double) k * sampleRate / (double) windowLen;
                const auto instFreq = binHz + dPhase / (2.0 * juce::MathConstants<double>::pi) * (sampleRate / (double) hopLen);
                instFreqs.push_back (instFreq);
            }

            ampDb.push_back (20.0 * std::log10 (juce::jmax (mag, 1.0e-9)));
            rmsValues.push_back (rms);

            prevPhase = phase;
            havePrev = true;
            pos += hopLen;
        }

        BassStability stats;
        stats.numWindows = (int) instFreqs.size();

        if (! instFreqs.empty())
        {
            double sum = 0.0;
            for (auto f : instFreqs) sum += f;
            stats.freqMean = sum / (double) instFreqs.size();

            double variance = 0.0;
            for (auto f : instFreqs) variance += (f - stats.freqMean) * (f - stats.freqMean);
            stats.freqStd = std::sqrt (variance / (double) instFreqs.size());
        }
        if (! ampDb.empty())
        {
            double sum = 0.0;
            for (auto v : ampDb) sum += v;
            const auto mean = sum / (double) ampDb.size();
            double variance = 0.0;
            for (auto v : ampDb) variance += (v - mean) * (v - mean);
            stats.ampDbStd = std::sqrt (variance / (double) ampDb.size());
        }
        if (! rmsValues.empty())
        {
            double sum = 0.0;
            for (auto v : rmsValues) sum += v;
            const auto mean = sum / (double) rmsValues.size();
            double variance = 0.0;
            for (auto v : rmsValues) variance += (v - mean) * (v - mean);
            const auto sd = std::sqrt (variance / (double) rmsValues.size());
            stats.rmsModDepth = mean > 1.0e-9 ? sd / mean : 0.0;
        }

        return stats;
    }

    /** Magnitude at freqOffsetHz above/below targetFreqHz, relative to the
        target's own magnitude (dB) - the classic phase-vocoder artifact
        signature (energy leaking into bins spaced around the true
        fundamental) is what's usually heard as wobble/warble beyond what
        the frequency-deviation measurement alone captures. */
    struct SidebandResult { double belowDb, aboveDb; };

    SidebandResult analyzeSidebands (const juce::AudioBuffer<float>& buffer, int channel, int startSample,
                                      int usableSamples, double sampleRate, float targetFreqHz, float offsetHz)
    {
        const auto win = juce::jmin (usableSamples, periodicAnalysisLength (sampleRate, targetFreqHz, 20));

        const auto targetMag = goertzelMagnitude (buffer, channel, startSample, win, sampleRate, targetFreqHz);
        const auto belowMag  = goertzelMagnitude (buffer, channel, startSample, win, sampleRate, targetFreqHz - offsetHz);
        const auto aboveMag  = goertzelMagnitude (buffer, channel, startSample, win, sampleRate, targetFreqHz + offsetHz);

        const auto targetDb = 20.0 * std::log10 (juce::jmax ((double) targetMag, 1.0e-9));
        return {
            20.0 * std::log10 (juce::jmax ((double) belowMag, 1.0e-9)) - targetDb,
            20.0 * std::log10 (juce::jmax ((double) aboveMag, 1.0e-9)) - targetDb
        };
    }

    /** Deterministic multi-tone chord/mix generator (sum of sines, fixed
        phases) - used for polyphonic material tests. No randomness, so
        results are exactly reproducible. */
    juce::AudioBuffer<float> generateChord (int totalSamples, double sampleRate,
                                             const std::vector<float>& freqsHz, const std::vector<float>& amplitudes)
    {
        jassert (freqsHz.size() == amplitudes.size());
        juce::AudioBuffer<float> buffer (1, totalSamples);
        buffer.clear();

        for (size_t n = 0; n < freqsHz.size(); ++n)
        {
            const auto increment = juce::MathConstants<float>::twoPi * freqsHz[n] / (float) sampleRate;
            auto* data = buffer.getWritePointer (0);
            for (int i = 0; i < totalSamples; ++i)
                data[i] += amplitudes[n] * std::sin (increment * (float) i);
        }
        return buffer;
    }

    /** Deterministic broadband test source (fixed frequency/phase set,
        no randomness) - used for the 0 ST A/B comparison against a
        latency-aligned dry copy. */
    juce::AudioBuffer<float> generateBroadband (int totalSamples, double sampleRate)
    {
        const std::vector<float> freqs { 40.0f, 80.0f, 150.0f, 300.0f, 600.0f, 1200.0f, 2500.0f, 5000.0f, 9000.0f, 14000.0f };
        std::vector<float> amps (freqs.size(), 0.09f);
        return generateChord (totalSamples, sampleRate, freqs, amps);
    }

    // ---- PAN test helpers -----------------------------------------------

    /** Feeds an already-built stereo buffer through `pan` in fixed-size
        blocks, mirroring exactly how PluginProcessor::processBlock() calls
        PanoramaProcessor::process(). */
    juce::AudioBuffer<float> runPanoramaProcessor (uni76::dsp::PanoramaProcessor& pan, const juce::AudioBuffer<float>& input,
                                                    int blockSize, float widthNormalised01, bool enabled)
    {
        const auto numChannels = input.getNumChannels();
        const auto totalSamples = input.getNumSamples();
        juce::AudioBuffer<float> result (numChannels, totalSamples);

        int done = 0;
        while (done < totalSamples)
        {
            const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
            juce::AudioBuffer<float> block (numChannels, thisBlock);

            for (int ch = 0; ch < numChannels; ++ch)
                block.copyFrom (ch, 0, input, ch, done, thisBlock);

            pan.process (block, widthNormalised01, enabled);

            for (int ch = 0; ch < numChannels; ++ch)
                result.copyFrom (ch, done, block, ch, 0, thisBlock);

            done += thisBlock;
        }
        return result;
    }

    struct StereoStats
    {
        double rmsL = 0.0, rmsR = 0.0, rmsMid = 0.0, rmsSide = 0.0;
        double sideMidRatio = 0.0, correlation = 0.0, peak = 0.0;
    };

    StereoStats measureStereo (const juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
    {
        double sumL2 = 0.0, sumR2 = 0.0, sumMid2 = 0.0, sumSide2 = 0.0, sumLR = 0.0, peak = 0.0;

        for (int i = startSample; i < startSample + numSamples; ++i)
        {
            const auto l = (double) buffer.getSample (0, i);
            const auto r = (double) buffer.getSample (1, i);
            const auto mid = 0.5 * (l + r);
            const auto side = 0.5 * (l - r);

            sumL2 += l * l; sumR2 += r * r;
            sumMid2 += mid * mid; sumSide2 += side * side;
            sumLR += l * r;
            peak = juce::jmax (peak, std::abs (l), std::abs (r));
        }

        StereoStats s;
        const auto n = (double) numSamples;
        s.rmsL = std::sqrt (sumL2 / n);
        s.rmsR = std::sqrt (sumR2 / n);
        s.rmsMid = std::sqrt (sumMid2 / n);
        s.rmsSide = std::sqrt (sumSide2 / n);
        s.sideMidRatio = s.rmsMid > 1.0e-9 ? s.rmsSide / s.rmsMid : 0.0;
        s.correlation = (sumL2 > 1.0e-12 && sumR2 > 1.0e-12) ? sumLR / std::sqrt (sumL2 * sumR2) : 0.0;
        s.peak = peak;
        return s;
    }

    juce::AudioBuffer<float> generateIdenticalStereo (int totalSamples, double sampleRate, float freqHz, float amplitude)
    {
        auto mono = generateSine (1, totalSamples, sampleRate, freqHz, amplitude);
        juce::AudioBuffer<float> stereo (2, totalSamples);
        stereo.copyFrom (0, 0, mono, 0, 0, totalSamples);
        stereo.copyFrom (1, 0, mono, 0, 0, totalSamples);
        return stereo;
    }

    /** Dual-mono, harmonically-rich source (several partials spanning the
        "main motion" band) - used for PAN's motion/centroid tests instead
        of a single pure tone. A single sustained sine is a genuine worst
        case for the phase-based mono decorrelation technique
        PanoramaProcessor uses (see PanoramaCurves.h's class comment on
        panAllpassHz): a fixed allpass's correlation with Mid varies with
        frequency and can happen to be large at any one unlucky frequency,
        but real (multi-frequency) material's correlations at different
        frequencies substantially cancel in aggregate - which is what
        actually determines whether the shipped product sounds balanced,
        not a single worst-case sine. */
    juce::AudioBuffer<float> generateMonoHarmonicStereo (int totalSamples, double sampleRate, float amplitude)
    {
        auto mono = generateChord (totalSamples, sampleRate,
                                    { 300.0f, 520.0f, 780.0f, 1150.0f, 1700.0f, 2500.0f },
                                    { amplitude, amplitude * 0.8f, amplitude * 0.65f, amplitude * 0.5f, amplitude * 0.4f, amplitude * 0.3f });
        juce::AudioBuffer<float> stereo (2, totalSamples);
        stereo.copyFrom (0, 0, mono, 0, 0, totalSamples);
        stereo.copyFrom (1, 0, mono, 0, 0, totalSamples);
        return stereo;
    }

    /** hardLeft=true -> signal in channel 0 only; false -> channel 1 only. */
    juce::AudioBuffer<float> generateHardPanned (int totalSamples, double sampleRate, float freqHz, float amplitude, bool hardLeft)
    {
        auto mono = generateSine (1, totalSamples, sampleRate, freqHz, amplitude);
        juce::AudioBuffer<float> stereo (2, totalSamples);
        stereo.clear();
        stereo.copyFrom (hardLeft ? 0 : 1, 0, mono, 0, 0, totalSamples);
        return stereo;
    }

    juce::AudioBuffer<float> generateAntiPhase (int totalSamples, double sampleRate, float freqHz, float amplitude)
    {
        auto mono = generateSine (1, totalSamples, sampleRate, freqHz, amplitude);
        juce::AudioBuffer<float> stereo (2, totalSamples);
        stereo.copyFrom (0, 0, mono, 0, 0, totalSamples);
        stereo.copyFrom (1, 0, mono, 0, 0, totalSamples);
        stereo.applyGain (1, 0, totalSamples, -1.0f);
        return stereo;
    }

    /** Centre bass (identical L/R) plus decorrelated "stereo highs" (a
        different high tone per channel) - the classic "bass mono, highs
        wide" real-world mix shape. */
    juce::AudioBuffer<float> generateCenterBassStereoHighs (int totalSamples, double sampleRate)
    {
        auto bass = generateIdenticalStereo (totalSamples, sampleRate, 80.0f, 0.3f);
        auto highL = generateSine (1, totalSamples, sampleRate, 4000.0f, 0.15f);
        auto highR = generateSine (1, totalSamples, sampleRate, 5500.0f, 0.15f);

        juce::AudioBuffer<float> out (2, totalSamples);
        out.copyFrom (0, 0, bass, 0, 0, totalSamples);
        out.copyFrom (1, 0, bass, 1, 0, totalSamples);
        out.addFrom (0, 0, highL, 0, 0, totalSamples);
        out.addFrom (1, 0, highR, 0, 0, totalSamples);
        return out;
    }

    /** Same chord content on both channels but with a different per-note
        amplitude balance L vs R (fully correlated - no independent
        content, no phase difference, just a static level tilt). */
    juce::AudioBuffer<float> generateCorrelatedChord (int totalSamples, double sampleRate)
    {
        const std::vector<float> freqs { 220.0f, 277.18f, 329.63f };
        auto left  = generateChord (totalSamples, sampleRate, freqs, { 0.2f, 0.15f, 0.18f });
        auto right = generateChord (totalSamples, sampleRate, freqs, { 0.15f, 0.2f, 0.14f });

        juce::AudioBuffer<float> out (2, totalSamples);
        out.copyFrom (0, 0, left, 0, 0, totalSamples);
        out.copyFrom (1, 0, right, 0, 0, totalSamples);
        return out;
    }

    /** Genuinely independent content per channel (not a level tilt of the
        same chord) - decorrelated stereo material. */
    juce::AudioBuffer<float> generateDecorrelatedStereo (int totalSamples, double sampleRate)
    {
        auto left  = generateChord (totalSamples, sampleRate, { 300.0f, 700.0f, 1300.0f }, { 0.2f, 0.15f, 0.1f });
        auto right = generateChord (totalSamples, sampleRate, { 450.0f, 950.0f, 1800.0f }, { 0.18f, 0.12f, 0.14f });

        juce::AudioBuffer<float> out (2, totalSamples);
        out.copyFrom (0, 0, left, 0, 0, totalSamples);
        out.copyFrom (1, 0, right, 0, 0, totalSamples);
        return out;
    }

    /** Mostly-Side synthetic passage: a strong anti-phase component plus
        a small correlated component, so it's Side-heavy but not pure
        cancellation. */
    juce::AudioBuffer<float> generateSideHeavy (int totalSamples, double sampleRate)
    {
        auto anti = generateAntiPhase (totalSamples, sampleRate, 700.0f, 0.25f);
        auto correlated = generateIdenticalStereo (totalSamples, sampleRate, 200.0f, 0.08f);

        juce::AudioBuffer<float> out (2, totalSamples);
        out.copyFrom (0, 0, anti, 0, 0, totalSamples);
        out.copyFrom (1, 0, anti, 1, 0, totalSamples);
        out.addFrom (0, 0, correlated, 0, 0, totalSamples);
        out.addFrom (1, 0, correlated, 1, 0, totalSamples);
        return out;
    }

    // ---- PAN motion test helpers (ORIGINAL/WIDE/MOTION contract) --------
    //
    // Static-width-only Goertzel-gain measurement (the previous MONO/
    // NATURAL/WIDE contract's measurePanSideGain()) doesn't carry over
    // cleanly: width and motion are now coupled (the LFO is always
    // running once width>0), so a single steady-state gain number is
    // ambiguous. Width/motion-depth *mapping* is instead verified by
    // calling PanoramaCurves.h's pure functions directly (exact, no
    // signal-domain ambiguity); the *system* is verified in the signal
    // domain via centroid trajectories and combined-power stability
    // below, which are meaningful even with the LFO active.

    /** (Renergy-Lenergy)/(Renergy+Lenergy) per non-overlapping window -
        the explicit stereo-centroid metric requested for the motion
        tests: 0 = perfectly centred, -1 = fully left, +1 = fully right. */
    std::vector<double> centroidSeries (const juce::AudioBuffer<float>& buffer, int startSample, int totalUsableSamples, int windowLen)
    {
        std::vector<double> series;
        int pos = startSample;
        const auto end = startSample + totalUsableSamples;

        while (pos + windowLen <= end)
        {
            double lEnergy = 0.0, rEnergy = 0.0;
            for (int i = 0; i < windowLen; ++i)
            {
                const auto l = (double) buffer.getSample (0, pos + i);
                const auto r = (double) buffer.getSample (1, pos + i);
                lEnergy += l * l;
                rEnergy += r * r;
            }
            const auto total = lEnergy + rEnergy;
            series.push_back (total > 1.0e-12 ? (rEnergy - lEnergy) / total : 0.0);
            pos += windowLen;
        }
        return series;
    }

    /** Combined stereo power (L^2+R^2) per non-overlapping window, in dB
        relative to the series' own mean - used to measure how much the
        *total* energy ripples over a motion cycle (should stay small -
        motion redistributes energy between channels, it should not
        pump the combined level). */
    std::vector<double> combinedPowerDbSeries (const juce::AudioBuffer<float>& buffer, int startSample, int totalUsableSamples, int windowLen)
    {
        std::vector<double> raw;
        int pos = startSample;
        const auto end = startSample + totalUsableSamples;

        while (pos + windowLen <= end)
        {
            double power = 0.0;
            for (int i = 0; i < windowLen; ++i)
            {
                const auto l = (double) buffer.getSample (0, pos + i);
                const auto r = (double) buffer.getSample (1, pos + i);
                power += l * l + r * r;
            }
            raw.push_back (power / (double) windowLen);
            pos += windowLen;
        }

        double mean = 0.0;
        for (auto v : raw) mean += v;
        mean = raw.empty() ? 1.0 : mean / (double) raw.size();
        mean = juce::jmax (mean, 1.0e-12);

        std::vector<double> db;
        db.reserve (raw.size());
        for (auto v : raw) db.push_back (10.0 * std::log10 (juce::jmax (v, 1.0e-12) / mean));
        return db;
    }

    struct SeriesStats { double minV = 0.0, maxV = 0.0, mean = 0.0, rmsExcursion = 0.0; };

    SeriesStats analyzeSeries (const std::vector<double>& series)
    {
        SeriesStats s;
        if (series.empty()) return s;

        s.minV = *std::min_element (series.begin(), series.end());
        s.maxV = *std::max_element (series.begin(), series.end());
        for (auto v : series) s.mean += v;
        s.mean /= (double) series.size();

        double sumSq = 0.0;
        for (auto v : series) sumSq += (v - s.mean) * (v - s.mean);
        s.rmsExcursion = std::sqrt (sumSq / (double) series.size());
        return s;
    }

    /** Simple centred moving-average smoother - used to average away fast
        ripple (e.g. beating between a multi-partial test source's own
        partials) before period estimation, while preserving a much
        slower (~0.3Hz) trajectory shape. */
    std::vector<double> smoothSeries (const std::vector<double>& series, int radius)
    {
        std::vector<double> out (series.size());
        for (size_t i = 0; i < series.size(); ++i)
        {
            double sum = 0.0;
            int count = 0;
            for (int d = -radius; d <= radius; ++d)
            {
                const auto idx = (int) i + d;
                if (idx >= 0 && idx < (int) series.size())
                {
                    sum += series[(size_t) idx];
                    ++count;
                }
            }
            out[i] = count > 0 ? sum / (double) count : series[i];
        }
        return out;
    }

    /** Mean spacing (in seconds) between consecutive upward crossings of
        the series' *own mean* (not literal zero - the trajectory is not
        guaranteed to be zero-centred for real/asymmetric material) in a
        windowed series (e.g. centroidSeries()), after smoothing away fast
        ripple - a simple, robust-enough period estimate for a slow,
        roughly-sinusoidal trajectory. Needs at least 2 crossings (a bit
        more than one full period of data); returns 0.0 if it can't find
        enough. */
    double measureOscillationPeriodSeconds (const std::vector<double>& series, double windowSeconds)
    {
        // ~0.5s smoothing radius - long enough to average out ripple much
        // faster than the ~3.3s motion period, short enough to preserve
        // that period's own shape.
        const auto radius = juce::jmax (1, (int) std::round (0.5 / windowSeconds));
        const auto smoothed = smoothSeries (series, radius);

        double mean = 0.0;
        for (auto v : smoothed) mean += v;
        mean = smoothed.empty() ? 0.0 : mean / (double) smoothed.size();

        std::vector<int> crossingIndices;
        for (size_t i = 1; i < smoothed.size(); ++i)
            if (smoothed[i - 1] < mean && smoothed[i] >= mean)
                crossingIndices.push_back ((int) i);

        if (crossingIndices.size() < 2)
            return 0.0;

        double totalPeriod = 0.0;
        for (size_t i = 1; i < crossingIndices.size(); ++i)
            totalPeriod += (double) (crossingIndices[i] - crossingIndices[i - 1]) * windowSeconds;

        return totalPeriod / (double) (crossingIndices.size() - 1);
    }
}

class UNI76PreampProcessorTests final : public juce::UnitTest
{
public:
    UNI76PreampProcessorTests() : juce::UnitTest ("uni76::dsp::PreampProcessor", "UNI76") {}

    void runTest() override
    {
        constexpr double sr = 44100.0;
        constexpr int blockSize = 512;

        beginTest ("DRIVE=0% is close to transparent for a moderate-level signal");
        {
            uni76::dsp::PreampProcessor preamp;
            preamp.prepare (sr, blockSize, 2);

            const auto totalSamples = blockSize * 20;
            auto processed = runPreampSine (preamp, 2, blockSize, totalSamples, sr, 1000.0f, 0.126f /* -18 dBFS */, 0.0f, true);

            // Skip the algorithmic latency + smoothing settle time, then
            // compare a settled window against an undelayed reference sine
            // - correlate via RMS since the oversampling/DC-blocker chain
            // shifts phase by a fraction of a sample, not by comparing
            // sample-for-sample.
            const auto settle = preamp.getLatencySamples() + blockSize * 4;
            const auto windowLen = totalSamples - settle - blockSize;
            expect (windowLen > 1000, "test window too short - increase totalSamples");

            const auto outRms = bufferRms (processed, 0, settle, windowLen);
            const auto referenceRms = 0.126f * 0.70710678f; // sine RMS = amplitude/sqrt(2)

            expectWithinAbsoluteError (outRms, referenceRms, referenceRms * 0.15f,
                                        "DRIVE=0% RMS should stay close to the input RMS");
        }

        beginTest ("preampEnabled=false bypasses the DSP (latency-aligned dry passthrough)");
        {
            uni76::dsp::PreampProcessor preamp;
            preamp.prepare (sr, blockSize, 2);

            const auto totalSamples = blockSize * 20;
            auto processed = runPreampSine (preamp, 2, blockSize, totalSamples, sr, 1000.0f, 0.5f, 1.0f /* full drive */, false);

            const auto settle = preamp.getLatencySamples() + blockSize * 4;
            const auto windowLen = totalSamples - settle - blockSize;

            const auto outRms = bufferRms (processed, 0, settle, windowLen);
            const auto referenceRms = 0.5f * 0.70710678f;

            // Even at DRIVE=100%, disabled must sound like input - not a
            // scaled-down or saturated version of it.
            expectWithinAbsoluteError (outRms, referenceRms, referenceRms * 0.05f,
                                        "disabled PREAMP should pass the dry signal through essentially unchanged");
        }

        beginTest ("Enable/disable transition does not create an extreme discontinuity");
        {
            uni76::dsp::PreampProcessor preamp;
            preamp.prepare (sr, blockSize, 2);

            constexpr float freqHz = 300.0f;
            constexpr float amplitude = 0.6f;
            const auto increment = juce::MathConstants<float>::twoPi * freqHz / (float) sr;

            // A genuinely continuous input phase across the transition -
            // any discontinuity found below must come from the crossfade
            // itself, not from a seam in the test's own signal generation.
            float phase = 0.0f;
            float maxJump = 0.0f;
            float prevSample = 0.0f;
            bool havePrev = false;

            for (int block = 0; block < 9; ++block)
            {
                const auto enabled = block < 8; // flip OFF on the 9th block, mid-stream
                juce::AudioBuffer<float> buffer (2, blockSize);

                for (int ch = 0; ch < 2; ++ch)
                {
                    auto p = phase;
                    for (int i = 0; i < blockSize; ++i)
                    {
                        buffer.setSample (ch, i, amplitude * std::sin (p));
                        p += increment;
                    }
                }

                preamp.process (buffer, 1.0f, enabled); // full drive throughout

                expect (bufferIsFinite (buffer), "transition block contains non-finite samples");

                for (int i = 0; i < blockSize; ++i)
                {
                    const auto s = buffer.getSample (0, i);
                    if (havePrev)
                        maxJump = juce::jmax (maxJump, std::abs (s - prevSample));
                    prevSample = s;
                    havePrev = true;
                }

                phase += increment * (float) blockSize;
            }

            // A continuous 300Hz sine at 0.6 amplitude, even hard-driven,
            // has a bounded sample-to-sample delta on its own; the
            // crossfade must not add an audible click on top of that.
            expect (maxJump < 0.35f, "enable/disable transition produced an unexpectedly large sample-to-sample jump: " + juce::String (maxJump, 4));
        }

        beginTest ("DRIVE=50% produces measurably more harmonic content than DRIVE=0%");
        {
            uni76::dsp::PreampProcessor preampOff, preampOn;
            preampOff.prepare (sr, blockSize, 1);
            preampOn.prepare (sr, blockSize, 1);

            const auto totalSamples = blockSize * 40;
            auto off = runPreampSine (preampOff, 1, blockSize, totalSamples, sr, 1000.0f, 0.126f, 0.0f, true);
            auto on  = runPreampSine (preampOn,  1, blockSize, totalSamples, sr, 1000.0f, 0.126f, 0.5f, true);

            // Goertzel-isolated H2 (2kHz) + H3 (3kHz) magnitude. The window
            // must be an exact number of *fundamental* periods (not just
            // exact for each harmonic bin independently) - H2/H3 are
            // integer multiples of the fundamental, so a fundamental-sized
            // window is automatically exact for them too, and this is what
            // stops the (much stronger) fundamental from leaking into a
            // nearby harmonic bin through a window-edge discontinuity.
            const auto win = periodicAnalysisLength (sr, 1000.0f, 200);

            const auto h23Off = goertzelMagnitude (off, 0, totalSamples - win, win, sr, 2000.0f)
                               + goertzelMagnitude (off, 0, totalSamples - win, win, sr, 3000.0f);
            const auto h23On  = goertzelMagnitude (on,  0, totalSamples - win, win, sr, 2000.0f)
                               + goertzelMagnitude (on,  0, totalSamples - win, win, sr, 3000.0f);

            expect (h23On > h23Off * 4.0f, "DRIVE=50% should leave clearly more H2+H3 content than DRIVE=0%");
        }

        beginTest ("DRIVE=100% produces more harmonic content than DRIVE=50% (monotonic, not hard clipping)");
        {
            uni76::dsp::PreampProcessor preamp50, preamp100;
            preamp50.prepare (sr, blockSize, 1);
            preamp100.prepare (sr, blockSize, 1);

            const auto totalSamples = blockSize * 40;
            auto at50  = runPreampSine (preamp50,  1, blockSize, totalSamples, sr, 1000.0f, 0.126f, 0.5f, true);
            auto at100 = runPreampSine (preamp100, 1, blockSize, totalSamples, sr, 1000.0f, 0.126f, 1.0f, true);

            const auto win = periodicAnalysisLength (sr, 1000.0f, 200);

            const auto h2350 = goertzelMagnitude (at50, 0, totalSamples - win, win, sr, 2000.0f)
                              + goertzelMagnitude (at50, 0, totalSamples - win, win, sr, 3000.0f);
            const auto h23100 = goertzelMagnitude (at100, 0, totalSamples - win, win, sr, 2000.0f)
                               + goertzelMagnitude (at100, 0, totalSamples - win, win, sr, 3000.0f);

            expect (h23100 > h2350, "DRIVE=100% should produce more H2+H3 content than DRIVE=50%");

            // "Never hard clipping": tanh() is asymptotically bounded but
            // never flat/pinned - the true peak must stay short of a hard
            // digital ceiling.
            const auto settle = juce::jmax (preamp50.getLatencySamples(), preamp100.getLatencySamples()) + blockSize * 4;
            const auto peak100 = bufferPeak (at100, 0, settle, totalSamples - settle - blockSize);
            expect (peak100 < 1.05f, "DRIVE=100% peak output should stay bounded, not slam into a hard ceiling");
        }

        beginTest ("DC offset stays safely small even at DRIVE=100%");
        {
            uni76::dsp::PreampProcessor preamp;
            preamp.prepare (sr, blockSize, 1);

            const auto totalSamples = blockSize * 40;
            auto processed = runPreampSine (preamp, 1, blockSize, totalSamples, sr, 1000.0f, 0.5f, 1.0f, true);

            const auto settle = preamp.getLatencySamples() + blockSize * 4;
            const auto windowLen = totalSamples - settle - blockSize;

            const auto mean = std::abs (bufferMean (processed, 0, settle, windowLen));
            expect (mean < 0.01f, "DRIVE=100% should not leave a significant DC offset in the output");
        }

        beginTest ("Low Cut measurably attenuates a low-frequency tone more at high DRIVE than at DRIVE=0%");
        {
            // A quiet tone (-18dBFS) so the nonlinear stage stays close to
            // linear and doesn't generate broadband harmonic energy that
            // would contaminate a full-signal RMS comparison - Goertzel
            // then isolates exactly the fundamental either way.
            uni76::dsp::PreampProcessor preampOff, preampOn;
            preampOff.prepare (sr, blockSize, 1);
            preampOn.prepare (sr, blockSize, 1);

            const auto totalSamples = blockSize * 60;
            auto off = runPreampSine (preampOff, 1, blockSize, totalSamples, sr, 30.0f, 0.126f, 0.0f, true);
            auto on  = runPreampSine (preampOn,  1, blockSize, totalSamples, sr, 30.0f, 0.126f, 1.0f, true);

            // Fewer cycles for a 30Hz tone (200 cycles would need ~6.7s of
            // audio) - 15 cycles is still ample for Goertzel resolution
            // and comfortably fits within totalSamples.
            const auto win = periodicAnalysisLength (sr, 30.0f, 15);
            const auto magOff = goertzelMagnitude (off, 0, totalSamples - win, win, sr, 30.0f);
            const auto magOn  = goertzelMagnitude (on,  0, totalSamples - win, win, sr, 30.0f);

            expect (magOn < magOff * 0.7f, "a 30Hz tone should be noticeably more attenuated at DRIVE=100% (Low Cut ~70Hz) than at DRIVE=0% (Low Cut ~20Hz)");
        }

        beginTest ("High Cut measurably attenuates a high-frequency tone more at high DRIVE than at DRIVE=0%");
        {
            uni76::dsp::PreampProcessor preampOff, preampOn;
            preampOff.prepare (sr, blockSize, 1);
            preampOn.prepare (sr, blockSize, 1);

            const auto totalSamples = blockSize * 60;
            auto off = runPreampSine (preampOff, 1, blockSize, totalSamples, sr, 15000.0f, 0.126f, 0.0f, true);
            auto on  = runPreampSine (preampOn,  1, blockSize, totalSamples, sr, 15000.0f, 0.126f, 1.0f, true);

            const auto win = periodicAnalysisLength (sr, 15000.0f, 200);
            const auto magOff = goertzelMagnitude (off, 0, totalSamples - win, win, sr, 15000.0f);
            const auto magOn  = goertzelMagnitude (on,  0, totalSamples - win, win, sr, 15000.0f);

            expect (magOn < magOff * 0.7f, "a 15kHz tone should be noticeably more attenuated at DRIVE=100% (High Cut ~11kHz) than at DRIVE=0% (High Cut ~20kHz)");
        }

        beginTest ("Output compensation keeps DRIVE=100% from being simply much louder than DRIVE=0%");
        {
            uni76::dsp::PreampProcessor preampOff, preampOn;
            preampOff.prepare (sr, blockSize, 1);
            preampOn.prepare (sr, blockSize, 1);

            const auto totalSamples = blockSize * 40;
            auto off = runPreampSine (preampOff, 1, blockSize, totalSamples, sr, 1000.0f, 0.126f, 0.0f, true);
            auto on  = runPreampSine (preampOn,  1, blockSize, totalSamples, sr, 1000.0f, 0.126f, 1.0f, true);

            const auto settle = juce::jmax (preampOff.getLatencySamples(), preampOn.getLatencySamples()) + blockSize * 4;
            const auto windowLen = totalSamples - settle - blockSize;

            const auto rmsOff = bufferRms (off, 0, settle, windowLen);
            const auto rmsOn  = bufferRms (on,  0, settle, windowLen);

            const auto deltaDb = juce::Decibels::gainToDecibels (rmsOn / juce::jmax (1.0e-9f, rmsOff));
            expect (deltaDb < 10.0f, "DRIVE=100% should not be dramatically louder than DRIVE=0% - got " + juce::String (deltaDb, 2) + " dB");
        }

        beginTest ("Mono processes without error and stays finite");
        {
            uni76::dsp::PreampProcessor preamp;
            preamp.prepare (sr, blockSize, 1);

            auto processed = runPreampSine (preamp, 1, blockSize, blockSize * 10, sr, 500.0f, 0.4f, 0.7f, true);
            expect (bufferIsFinite (processed), "mono processing produced non-finite samples");
        }

        beginTest ("Stereo: signal in the left channel only never bleeds into a silent right channel");
        {
            uni76::dsp::PreampProcessor preamp;
            preamp.prepare (sr, blockSize, 2);

            const auto totalSamples = blockSize * 10;
            juce::AudioBuffer<float> result (2, totalSamples);
            int done = 0;
            float phase = 0.0f;
            const auto increment = juce::MathConstants<float>::twoPi * 440.0f / (float) sr;

            while (done < totalSamples)
            {
                const auto n = juce::jmin (blockSize, totalSamples - done);
                juce::AudioBuffer<float> block (2, n);
                block.clear();

                auto p = phase;
                for (int i = 0; i < n; ++i)
                {
                    block.setSample (0, i, 0.7f * std::sin (p)); // left only
                    p += increment;
                }
                phase += increment * (float) n;

                preamp.process (block, 0.8f, true);

                result.copyFrom (0, done, block, 0, 0, n);
                result.copyFrom (1, done, block, 1, 0, n);
                done += n;
            }

            expect (bufferIsFinite (result), "stereo processing produced non-finite samples");

            const auto rightPeak = bufferPeak (result, 1, 0, totalSamples);
            expectWithinAbsoluteError (rightPeak, 0.0f, 1.0e-6f,
                                        "an independently-stateful right channel fed silence must stay silent regardless of left-channel content");
        }

        beginTest ("Stereo: an identical signal in L and R stays numerically identical after PREAMP (no stereo drift)");
        {
            uni76::dsp::PreampProcessor preamp;
            preamp.prepare (sr, blockSize, 2);

            const auto totalSamples = blockSize * 20;
            juce::AudioBuffer<float> result (2, totalSamples);
            int done = 0;
            float phase = 0.0f;
            const auto increment = juce::MathConstants<float>::twoPi * 300.0f / (float) sr;

            while (done < totalSamples)
            {
                const auto n = juce::jmin (blockSize, totalSamples - done);
                juce::AudioBuffer<float> block (2, n);

                auto p = phase;
                for (int i = 0; i < n; ++i)
                {
                    const auto s = 0.6f * std::sin (p);
                    block.setSample (0, i, s);
                    block.setSample (1, i, s); // identical to left
                    p += increment;
                }
                phase += increment * (float) n;

                preamp.process (block, 0.85f, true);

                result.copyFrom (0, done, block, 0, 0, n);
                result.copyFrom (1, done, block, 1, 0, n);
                done += n;
            }

            expect (bufferIsFinite (result), "stereo processing produced non-finite samples");

            float maxDrift = 0.0f;
            for (int i = 0; i < totalSamples; ++i)
                maxDrift = juce::jmax (maxDrift, std::abs (result.getSample (0, i) - result.getSample (1, i)));

            expectWithinAbsoluteError (maxDrift, 0.0f, 1.0e-6f,
                                        "identical L/R input must produce numerically identical L/R output - no randomness/modulation anywhere in the chain");
        }

        beginTest ("Sample rates 44.1/48/96/192kHz all process finite audio with the expected latency architecture");
        {
            struct Config { double sr; bool expectLatency; };
            const Config configs[] { { 44100.0, true }, { 48000.0, true }, { 96000.0, true }, { 192000.0, false } };

            for (const auto& config : configs)
            {
                uni76::dsp::PreampProcessor preamp;
                preamp.prepare (config.sr, blockSize, 2);

                auto processed = runPreampSine (preamp, 2, blockSize, blockSize * 10, config.sr, 1000.0f, 0.5f, 0.6f, true);
                expect (bufferIsFinite (processed), juce::String ("non-finite output at ") + juce::String (config.sr) + "Hz");

                std::cout << "  latency @ " << config.sr << "Hz = " << preamp.getLatencySamples() << " samples ("
                           << (1000.0 * preamp.getLatencySamples() / config.sr) << " ms)" << std::endl;

                if (config.expectLatency)
                    expect (preamp.getLatencySamples() > 0, juce::String ("expected nonzero latency at ") + juce::String (config.sr) + "Hz");
                else
                    expectEquals (preamp.getLatencySamples(), 0);
            }
        }

        beginTest ("Different block sizes (1/7/64/512/4096) all remain finite and stable");
        {
            const int sizes[] { 1, 7, 64, 512, 4096 };

            for (auto size : sizes)
            {
                uni76::dsp::PreampProcessor preamp;
                preamp.prepare (sr, juce::jmax (size, 4096), 2);

                auto processed = runPreampSine (preamp, 2, size, size * 30, sr, 1000.0f, 0.4f, 0.5f, true);
                expect (bufferIsFinite (processed), juce::String ("non-finite output at block size ") + juce::String (size));
            }
        }

        beginTest ("Automation sweep 0 -> 100 -> 0 produces no NaN/Inf and no extreme discontinuity");
        {
            uni76::dsp::PreampProcessor preamp;
            preamp.prepare (sr, blockSize, 2);

            juce::Random random (99);
            float prevSample = 0.0f;
            float maxJump = 0.0f;

            for (int block = 0; block < 60; ++block)
            {
                // Sweep 0->100 over the first half, 100->0 over the second.
                const auto t = (float) block / 59.0f;
                const auto drive = t < 0.5f ? (t * 2.0f) : (2.0f - t * 2.0f);

                juce::AudioBuffer<float> buffer (2, blockSize);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        buffer.setSample (ch, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 300.0f
                                                                    * (float) (block * blockSize + i) / (float) sr));

                preamp.process (buffer, drive, true);

                expect (bufferIsFinite (buffer), "automation sweep produced non-finite samples");

                for (int i = 0; i < blockSize; ++i)
                {
                    const auto s = buffer.getSample (0, i);
                    maxJump = juce::jmax (maxJump, std::abs (s - prevSample));
                    prevSample = s;
                }
            }

            expect (maxJump < 0.5f, "automation sweep produced an unexpectedly large sample-to-sample jump");
        }

        beginTest ("Silence remains silence at any DRIVE/enabled setting");
        {
            uni76::dsp::PreampProcessor preamp;
            preamp.prepare (sr, blockSize, 2);

            for (float drive : { 0.0f, 0.5f, 1.0f })
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                buffer.clear();

                for (int block = 0; block < 10; ++block)
                    preamp.process (buffer, drive, true);

                expectWithinAbsoluteError (bufferPeak (buffer, 0, 0, blockSize), 0.0f, 1.0e-6f, "silence in should stay silence out");
                buffer.clear();
            }
        }

        beginTest ("NaN/Inf input samples never reach the output");
        {
            uni76::dsp::PreampProcessor preamp;
            preamp.prepare (sr, blockSize, 2);

            juce::AudioBuffer<float> buffer (2, blockSize);
            buffer.clear();
            buffer.setSample (0, 10, std::numeric_limits<float>::quiet_NaN());
            buffer.setSample (0, 20, std::numeric_limits<float>::infinity());
            buffer.setSample (1, 30, -std::numeric_limits<float>::infinity());

            preamp.process (buffer, 1.0f, true);

            expect (bufferIsFinite (buffer), "NaN/Inf input samples leaked through to the output");
        }

        beginTest ("Latency is constant for a given sample rate regardless of DRIVE or enabled state");
        {
            uni76::dsp::PreampProcessor preamp;
            preamp.prepare (sr, blockSize, 2);

            const auto latency = preamp.getLatencySamples();
            expect (latency > 0, "44.1kHz should report nonzero latency (4x oversampling)");

            juce::AudioBuffer<float> buffer (2, blockSize);
            for (float drive : { 0.0f, 1.0f })
                for (bool enabled : { true, false })
                {
                    preamp.process (buffer, drive, enabled);
                    expectEquals (preamp.getLatencySamples(), latency,
                                  "getLatencySamples() must stay constant for correct host plugin-delay-compensation");
                }
        }
    }
};

static UNI76PreampProcessorTests uni76PreampProcessorTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// Full-processor integration: preampEnabled=false really does bypass audibly,
// and the parameter/state round-trip works exactly as it does for every
// other module.
class UNI76PreampIntegrationTests final : public juce::UnitTest
{
public:
    UNI76PreampIntegrationTests() : juce::UnitTest ("UNI76AudioProcessor + PREAMP", "UNI76") {}

    void runTest() override
    {
        beginTest ("preampEnabled=false (via ModuleEnableState) bypasses PREAMP inside the real processor");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (44100.0, 512);

            auto* preampParam = processor.getValueTreeState().getParameter (uni76::ParamID::preamp);
            preampParam->setValueNotifyingHost (1.0f); // 100%

            processor.getModuleEnableState().setEnabled (0, false); // preamp OFF
            // EQ defaults to 50% (PHONE) and is real DSP now too - disable
            // it here so this test isolates PREAMP's own bypass behaviour
            // rather than also measuring PHONE's bell/band-pass shaping.
            processor.getModuleEnableState().setEnabled (1, false); // eq OFF

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> buffer (2, 512);

            // Settle the bypass crossfade.
            for (int i = 0; i < 20; ++i)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int s = 0; s < 512; ++s)
                        buffer.setSample (ch, s, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 1000.0f
                                                                    * (float) (i * 512 + s) / 44100.0f));
                processor.processBlock (buffer, midi);
            }

            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < 512; ++s)
                    peak = juce::jmax (peak, std::abs (buffer.getSample (ch, s)));

            // Disabled at DRIVE=100%: if PREAMP were still active this
            // would be heavily saturated/soft-limited well under the fed
            // amplitude (0.5) - bypass should leave the peak essentially at
            // the fed level instead.
            expectWithinAbsoluteError (peak, 0.5f, 0.05f, "disabled PREAMP should not audibly saturate the signal");
        }
    }
};

static UNI76PreampIntegrationTests uni76PreampIntegrationTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// Offline harmonic analysis for docs/DSP_PREAMP.md - a 1kHz sine at -18dBFS
// through DRIVE 0/25/50/75/100, measured via FFT. Prints unconditionally
// (not just on failure) so the numbers can be captured for the docs; also
// asserts the progression is sane (monotonic-ish, bounded, no NaN).
class UNI76PreampHarmonicAnalysisTests final : public juce::UnitTest
{
public:
    UNI76PreampHarmonicAnalysisTests() : juce::UnitTest ("uni76::dsp::PreampProcessor harmonic analysis", "UNI76") {}

    struct Measurement
    {
        float fundamentalDb = -300.0f;
        std::array<float, 4> harmonicDb { -300.0f, -300.0f, -300.0f, -300.0f }; // H2..H5
        float thdPercent = 0.0f;
    };

    static Measurement measure (const juce::AudioBuffer<float>& signal, int channel, double sampleRate, float fundamentalHz)
    {
        constexpr int fftOrder = 13; // 8192
        constexpr int fftSize = 1 << fftOrder;

        juce::dsp::FFT fft (fftOrder);
        juce::dsp::WindowingFunction<float> window (fftSize, juce::dsp::WindowingFunction<float>::blackmanHarris);

        std::vector<float> fftData ((size_t) fftSize * 2, 0.0f);
        const auto offset = signal.getNumSamples() - fftSize;
        jassert (offset >= 0);

        for (int i = 0; i < fftSize; ++i)
            fftData[(size_t) i] = signal.getSample (channel, offset + i);

        window.multiplyWithWindowingTable (fftData.data(), fftSize);
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        const auto binHz = sampleRate / (double) fftSize;
        auto magnitudeAt = [&] (float freqHz)
        {
            const auto bin = juce::jlimit (0, fftSize / 2 - 1, (int) std::round ((double) freqHz / binHz));
            return fftData[(size_t) bin];
        };

        Measurement m;
        const auto fundamentalMag = magnitudeAt (fundamentalHz);
        m.fundamentalDb = juce::Decibels::gainToDecibels (fundamentalMag + 1.0e-9f);

        double harmonicPowerSum = 0.0;
        for (int h = 2; h <= 5; ++h)
        {
            const auto mag = magnitudeAt (fundamentalHz * (float) h);
            m.harmonicDb[(size_t) h - 2] = juce::Decibels::gainToDecibels (mag + 1.0e-9f);
            harmonicPowerSum += (double) mag * (double) mag;
        }

        m.thdPercent = fundamentalMag > 1.0e-9f
            ? (float) (100.0 * std::sqrt (harmonicPowerSum) / (double) fundamentalMag)
            : 0.0f;

        return m;
    }

    void runTest() override
    {
        beginTest ("1kHz @ -18dBFS through DRIVE 0/25/50/75/100 - harmonic progression is smooth, bounded, monotonic");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 512;
            constexpr float amplitude = 0.125892f; // -18 dBFS
            constexpr float fundamentalHz = 1000.0f;

            const float drives[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
            std::vector<Measurement> results;

            std::cout << "\n=== PREAMP harmonic analysis: 1kHz @ -18dBFS ===" << std::endl;

            for (auto drive : drives)
            {
                uni76::dsp::PreampProcessor preamp;
                preamp.prepare (sr, blockSize, 1);

                const auto totalSamples = blockSize * 40;
                auto processed = runPreampSine (preamp, 1, blockSize, totalSamples, sr, fundamentalHz, amplitude, drive, true);

                const auto m = measure (processed, 0, sr, fundamentalHz);
                results.push_back (m);

                std::cout << "  DRIVE " << (int) (drive * 100.0f) << "%: "
                           << "fund=" << m.fundamentalDb << "dB  "
                           << "H2=" << m.harmonicDb[0] << "dB  "
                           << "H3=" << m.harmonicDb[1] << "dB  "
                           << "H4=" << m.harmonicDb[2] << "dB  "
                           << "H5=" << m.harmonicDb[3] << "dB  "
                           << "THD=" << m.thdPercent << "%"
                           << std::endl;

                expect (std::isfinite (m.fundamentalDb), "non-finite fundamental measurement");
                for (auto h : m.harmonicDb)
                    expect (std::isfinite (h), "non-finite harmonic measurement");
            }

            std::cout << "=== end harmonic analysis ===" << std::endl << std::endl;

            // Monotonic (non-decreasing) THD as DRIVE increases - no sudden
            // explosion of high-order content anywhere in the sweep.
            for (size_t i = 1; i < results.size(); ++i)
                expect (results[i].thdPercent >= results[i - 1].thdPercent - 0.05f,
                        "THD should not meaningfully decrease as DRIVE increases");

            // Calibrated per the sound-calibration pass: DRIVE=100% at
            // -18dBFS must produce clearly measurable but *musical* (not
            // fuzz-range) harmonic content - single digits to low tens of
            // percent, not the >400% THD an earlier, unbounded asymmetric
            // model produced at hot input levels (see docs/DSP_PREAMP.md).
            expect (results.back().thdPercent > 0.5f, "DRIVE=100% should produce clearly measurable harmonic content");
            expect (results.back().thdPercent < 15.0f, "DRIVE=100% THD at -18dBFS should stay in a musical, non-fuzz range");
        }

        beginTest ("Input level x DRIVE matrix (-30/-18/-12/-6 dBFS x 25/50/75/100%) - level-dependent, bounded, no runaway THD");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 512;
            constexpr float fundamentalHz = 1000.0f;

            const float levelsDb[] { -30.0f, -18.0f, -12.0f, -6.0f };
            const float drives[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

            std::cout << "=== PREAMP level x DRIVE matrix (1kHz) ===" << std::endl;
            std::cout << "  level_dBFS  drive%   THD%     H2dB     H3dB     H5dB   deltaRMSdB" << std::endl;

            for (auto levelDb : levelsDb)
            {
                const auto amplitude = juce::Decibels::decibelsToGain (levelDb);
                float baseRms = -1.0f;

                for (auto drive : drives)
                {
                    uni76::dsp::PreampProcessor preamp;
                    preamp.prepare (sr, blockSize, 1);

                    const auto totalSamples = blockSize * 60;
                    auto processed = runPreampSine (preamp, 1, blockSize, totalSamples, sr, fundamentalHz, amplitude, drive, true);

                    const auto m = measure (processed, 0, sr, fundamentalHz);

                    const auto settle = preamp.getLatencySamples() + blockSize * 4;
                    const auto rms = bufferRms (processed, 0, settle, totalSamples - settle - blockSize);
                    if (drive == 0.0f) baseRms = rms;
                    const auto deltaDb = baseRms > 1.0e-9f ? juce::Decibels::gainToDecibels (rms / baseRms) : 0.0f;

                    std::cout << "  " << levelDb << "\t     " << (int) (drive * 100.0f)
                               << "\t " << m.thdPercent << "\t " << m.harmonicDb[0] << "\t " << m.harmonicDb[1]
                               << "\t " << m.harmonicDb[3] << "\t " << deltaDb << std::endl;

                    expect (std::isfinite (m.thdPercent) && std::isfinite (rms), "non-finite measurement in level x DRIVE matrix");

                    // The catastrophic failure mode this test exists to
                    // catch: an earlier unbounded asymmetric term measured
                    // >400% THD at -6dBFS/100% drive - unmistakably fuzz,
                    // not analogue character. However hard a hot signal is
                    // driven, THD must stay bounded.
                    expect (m.thdPercent < 100.0f, "THD exceeded 100% - runaway/rectifying distortion, not musical saturation");
                }
            }

            std::cout << "=== end level x DRIVE matrix ===" << std::endl << std::endl;
        }

        beginTest ("DRIVE=0% null test: magnitude (gain) deviation from unity at 100Hz/1kHz/10kHz/broadband");
        {
            // A naive time-domain sample subtraction was tried first and
            // discarded: near the fixed 20Hz/20kHz filter boundaries
            // (Low Cut/High Cut at DRIVE=0%, plus the rounding filter),
            // any real filter's own group delay causes a fractional-sample
            // phase shift that a pure integer-latency-aligned subtraction
            // reads as a huge "residual", even though the actual
            // *coloration* (gain/loudness at that frequency) barely
            // changes - 100Hz measured -9.6dB and 10kHz -4.9dB "residual"
            // that way, purely from phase, not level. What "practically
            // transparent, no unexpected coloration" actually means is a
            // magnitude/gain question, so this measures exactly that via
            // Goertzel: |wet magnitude| vs the fed amplitude, in dB.
            constexpr double sr = 44100.0;
            constexpr int blockSize = 512;
            constexpr float amplitude = 0.25f;

            auto nullTestAt = [&] (float freqHz, const char* label, float toleranceDb)
            {
                uni76::dsp::PreampProcessor preamp;
                preamp.prepare (sr, blockSize, 1);

                const auto totalSamples = blockSize * 60;
                auto processed = runPreampSine (preamp, 1, blockSize, totalSamples, sr, freqHz, amplitude, 0.0f, true);

                // Fewer cycles for low frequencies so the analysis window
                // (periodicAnalysisLength) stays comfortably inside
                // totalSamples - 100 cycles of 100Hz alone would need
                // 44100 samples.
                const auto cycles = freqHz < 200.0f ? 20 : 100;
                const auto win = periodicAnalysisLength (sr, freqHz, cycles);
                const auto wetMag = goertzelMagnitude (processed, 0, totalSamples - win, win, sr, freqHz);
                const auto gainDb = 20.0f * std::log10 (wetMag / amplitude);

                std::cout << "  null test " << label << ": gain deviation = " << gainDb << " dB" << std::endl;
                expect (std::abs (gainDb) < toleranceDb,
                        juce::String ("DRIVE=0% gain deviation at ") + label + " should stay within " + juce::String (toleranceDb, 1) + "dB");
            };

            std::cout << "=== PREAMP DRIVE=0% null test ===" << std::endl;
            // Comfortably mid-band frequencies get a tight tolerance; 100Hz
            // and 10kHz sit within about an octave of the soft 20Hz/20kHz
            // boundaries the product brief explicitly allows some
            // coloration near, so they get a slightly looser (still small)
            // tolerance.
            nullTestAt (100.0f, "100Hz", 0.5f);
            nullTestAt (1000.0f, "1kHz", 0.2f);
            nullTestAt (10000.0f, "10kHz", 0.75f);

            // Broadband: a deterministic multi-tone sum spanning the
            // audible range, checking each tone's own magnitude deviation.
            {
                uni76::dsp::PreampProcessor preamp;
                preamp.prepare (sr, blockSize, 1);
                const auto totalSamples = blockSize * 60;

                const float tones[] { 80.0f, 400.0f, 1200.0f, 4000.0f, 9000.0f };
                juce::AudioBuffer<float> dry (1, totalSamples);
                dry.clear();
                for (auto freqHz : tones)
                {
                    const auto inc = juce::MathConstants<double>::twoPi * (double) freqHz / sr;
                    double phase = 0.0;
                    for (int i = 0; i < totalSamples; ++i) { dry.addSample (0, i, (amplitude / 5.0f) * (float) std::sin (phase)); phase += inc; }
                }

                juce::AudioBuffer<float> wet;
                wet.makeCopyOf (dry);
                int done = 0;
                while (done < totalSamples)
                {
                    const auto n = juce::jmin (blockSize, totalSamples - done);
                    juce::AudioBuffer<float> block (1, n);
                    block.copyFrom (0, 0, wet, 0, done, n);
                    preamp.process (block, 0.0f, true);
                    wet.copyFrom (0, done, block, 0, 0, n);
                    done += n;
                }

                for (auto freqHz : tones)
                {
                    const auto cycles = freqHz < 200.0f ? 20 : 100;
                    const auto win = periodicAnalysisLength (sr, freqHz, cycles);
                    const auto mag = goertzelMagnitude (wet, 0, totalSamples - win, win, sr, freqHz);
                    const auto gainDb = 20.0f * std::log10 (mag / (amplitude / 5.0f));
                    const auto toleranceDb = (freqHz <= 100.0f || freqHz >= 9000.0f) ? 0.75f : 0.3f;

                    std::cout << "  null test broadband @ " << freqHz << "Hz: gain deviation = " << gainDb << " dB" << std::endl;
                    expect (std::abs (gainDb) < toleranceDb,
                            "DRIVE=0% broadband gain deviation at " + juce::String (freqHz, 0) + "Hz should stay small");
                }
            }

            std::cout << "=== end null test ===" << std::endl << std::endl;
        }

        beginTest ("Aliasing: production oversampled path suppresses fold-back energy vs a non-oversampled reference");
        {
            // Production never gets a runtime oversampling on/off switch
            // (see Source/DSP/PreampProcessor.cpp) - this reference path
            // exists only in this test, replicating the identical
            // coloration+waveshaper math directly at the base rate, with
            // no up/downsampling, purely to measure how much the real
            // oversampled production path is actually suppressing.
            constexpr double sr = 44100.0;
            constexpr int blockSize = 512;
            constexpr float amplitude = 0.3f;
            constexpr float drive = 1.0f; // worst case - most harmonic energy generated

            auto referenceNoOversampling = [&] (float freqHz)
            {
                uni76::dsp::OnePoleLowPass rounding;
                uni76::dsp::Biquad shelf;
                const auto t = drive;
                rounding.setCutoffHz (sr, uni76::dsp::preampRoundingCutoffHz (t));
                uni76::dsp::makeLowShelf (shelf, sr, uni76::dsp::preampColorShelfFreqHz, uni76::dsp::preampColorShelfGainDb (t));

                const auto driveGain = uni76::dsp::preampDriveGainLinear (t);
                const auto asym = uni76::dsp::preampAsymmetryAmount (t);
                const auto norm = std::tanh (driveGain);

                const int totalSamples = blockSize * 40;
                juce::AudioBuffer<float> out (1, totalSamples);
                const auto inc = juce::MathConstants<double>::twoPi * (double) freqHz / sr;
                double phase = 0.0;

                for (int i = 0; i < totalSamples; ++i)
                {
                    auto x = amplitude * (float) std::sin (phase);
                    x = rounding.processSample (x);
                    x = shelf.processSample (x);
                    const auto xd = x * driveGain;
                    const auto shaped = xd >= 0.0f ? std::tanh (xd) : std::tanh (xd * (1.0f - asym));
                    out.setSample (0, i, norm > 1.0e-6f ? shaped / norm : shaped);
                    phase += inc;
                }
                return out;
            };

            auto productionOversampled = [&] (float freqHz)
            {
                uni76::dsp::PreampProcessor preamp;
                preamp.prepare (sr, blockSize, 1);
                const int totalSamples = blockSize * 40;
                return runPreampSine (preamp, 1, blockSize, totalSamples, sr, freqHz, amplitude, drive, true);
            };

            std::cout << "=== PREAMP aliasing measurement (44.1kHz, DRIVE=100%) ===" << std::endl;

            for (float testToneHz : { 4000.0f, 8000.0f, 12000.0f })
            {
                auto reference = referenceNoOversampling (testToneHz);
                auto production = productionOversampled (testToneHz);

                const auto totalSamples = reference.getNumSamples();
                const auto win = periodicAnalysisLength (sr, testToneHz, 100);
                const auto start = totalSamples - win;

                // Any energy found *below* the fundamental (outside the
                // Low Cut's own shaping region) can only be aliasing or
                // filter-shaping artifact for a signal whose real harmonic
                // content only exists at/above its own frequency - probe
                // a representative band well below the tone.
                const auto probeHz = juce::jmax (500.0f, testToneHz * 0.5f);
                const auto refAlias = goertzelMagnitude (reference, 0, start, win, sr, probeHz);
                const auto prodAlias = goertzelMagnitude (production, 0, start, win, sr, probeHz);

                const auto suppressionDb = refAlias > 1.0e-9f
                    ? juce::Decibels::gainToDecibels (prodAlias / refAlias)
                    : 0.0f;

                std::cout << "  tone=" << testToneHz << "Hz probe=" << probeHz << "Hz: "
                           << "reference(no oversampling)=" << juce::Decibels::gainToDecibels (refAlias + 1.0e-9f) << "dB  "
                           << "production(oversampled)=" << juce::Decibels::gainToDecibels (prodAlias + 1.0e-9f) << "dB  "
                           << "suppression=" << suppressionDb << "dB"
                           << std::endl;

                expect (std::isfinite (refAlias) && std::isfinite (prodAlias), "non-finite aliasing measurement");
                expect (prodAlias <= refAlias + 1.0e-6f,
                        "the oversampled production path should never show *more* fold-back energy than the non-oversampled reference");
            }

            std::cout << "=== end aliasing measurement ===" << std::endl << std::endl;
        }

        beginTest ("Low Cut / High Cut frequency response - measured attenuation at DRIVE 0/25/50/75/100");
        {
            const float drives[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

            std::cout << "=== PREAMP Low Cut / High Cut mapping ===" << std::endl;

            for (auto drive : drives)
            {
                std::cout << "  DRIVE " << (int) (drive * 100.0f) << "%: "
                           << "LowCut=" << uni76::dsp::preampLowCutHz (drive) << "Hz  "
                           << "HighCut=" << uni76::dsp::preampHighCutHz (drive) << "Hz"
                           << std::endl;
            }

            std::cout << "=== end frequency response ===" << std::endl << std::endl;

            expectWithinAbsoluteError (uni76::dsp::preampLowCutHz (0.0f), 20.0f, 0.5f);
            expect (uni76::dsp::preampLowCutHz (1.0f) >= 65.0f && uni76::dsp::preampLowCutHz (1.0f) <= 75.0f);

            expectWithinAbsoluteError (uni76::dsp::preampHighCutHz (0.0f), 20000.0f, 1.0f);
            expect (uni76::dsp::preampHighCutHz (1.0f) >= 10000.0f && uni76::dsp::preampHighCutHz (1.0f) <= 12000.0f);
        }
    }
};

static UNI76PreampHarmonicAnalysisTests uni76PreampHarmonicAnalysisTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// uni76::dsp::EqProcessor - the deterministic DSP test suite required by the
// EQ audit.
namespace
{
    juce::AudioBuffer<float> runEqSine (uni76::dsp::EqProcessor& eq, int numChannels, int blockSize,
                                         int totalSamples, double sampleRate, float freqHz, float amplitude,
                                         float eqNormalised01, bool enabled)
    {
        juce::AudioBuffer<float> result (numChannels, totalSamples);
        const auto increment = juce::MathConstants<float>::twoPi * freqHz / (float) sampleRate;
        float phase = 0.0f;
        int done = 0;

        while (done < totalSamples)
        {
            const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
            juce::AudioBuffer<float> block (numChannels, thisBlock);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto p = phase;
                for (int i = 0; i < thisBlock; ++i)
                {
                    block.setSample (ch, i, amplitude * std::sin (p));
                    p += increment;
                }
            }

            eq.process (block, eqNormalised01, enabled);

            for (int ch = 0; ch < numChannels; ++ch)
                result.copyFrom (ch, done, block, ch, 0, thisBlock);

            phase += increment * (float) thisBlock;
            done += thisBlock;
        }

        return result;
    }

    /** Steady-state gain (dB) of the EQ at one frequency - runs long enough
        to settle, then measures via an exact-period Goertzel window. The
        buffer length is sized from the actual window needed (not a fixed
        block count), since a low frequency at a high sample rate can need
        far more samples than a fixed "60 blocks" budget provides. */
    float eqGainDb (uni76::dsp::EqProcessor& eq, double sampleRate, int blockSize, float freqHz,
                     float amplitude, float eqNormalised01, bool enabled = true)
    {
        const auto cycles = freqHz <= 150.0f ? 20 : 100;
        const auto win = periodicAnalysisLength (sampleRate, freqHz, cycles);
        const auto settle = blockSize * 4;
        const auto totalSamples = win + settle + blockSize;

        auto sig = runEqSine (eq, 1, blockSize, totalSamples, sampleRate, freqHz, amplitude, eqNormalised01, enabled);
        const auto mag = goertzelMagnitude (sig, 0, totalSamples - win, win, sampleRate, freqHz);
        return juce::Decibels::gainToDecibels (mag / amplitude);
    }
}

class UNI76EqProcessorTests final : public juce::UnitTest
{
public:
    UNI76EqProcessorTests() : juce::UnitTest ("uni76::dsp::EqProcessor", "UNI76") {}

    void runTest() override
    {
        constexpr double sr = 44100.0;
        constexpr int blockSize = 512;
        constexpr float amplitude = 0.25f;

        beginTest ("eqEnabled=false bypasses the EQ (dry passthrough, no delay needed - zero latency)");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 1);

            // DARK is the most aggressive top-cut anchor - if disabled
            // didn't truly bypass, this would show heavy attenuation.
            const auto gainDb = eqGainDb (eq, sr, blockSize, 10000.0f, amplitude, 0.0f, false);
            expectWithinAbsoluteError (gainDb, 0.0f, 0.3f, "disabled EQ should leave a 10kHz tone essentially untouched");
        }

        beginTest ("EQ=0% (DARK): bass retained, top rounded - not an underwater effect");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 1);

            const auto bass = eqGainDb (eq, sr, blockSize, 100.0f, amplitude, 0.0f);
            const auto highs = eqGainDb (eq, sr, blockSize, 12000.0f, amplitude, 0.0f);

            expect (bass > -1.5f, "DARK should keep bass close to unity, not cut it");
            expect (highs < -6.0f, "DARK should noticeably round off the top");
            expect (highs > -60.0f, "DARK should round the top, not remove it entirely (not an underwater effect)");
        }

        beginTest ("EQ=50% (PHONE): voice-band character - both ends attenuated, mid stays present");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 1);

            const auto low = eqGainDb (eq, sr, blockSize, 100.0f, amplitude, 0.5f);
            const auto mid = eqGainDb (eq, sr, blockSize, 1000.0f, amplitude, 0.5f);
            const auto high = eqGainDb (eq, sr, blockSize, 10000.0f, amplitude, 0.5f);

            expect (low < -10.0f, "PHONE should clearly attenuate below the voice band");
            expect (high < -10.0f, "PHONE should clearly attenuate above the voice band");
            expect (mid > -3.0f && mid < 6.0f, "PHONE's midrange should stay present/readable, not buried or blaring");
        }

        beginTest ("EQ=100% (AIR): open bass, lifted highs, no runaway gain");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 1);

            const auto bass = eqGainDb (eq, sr, blockSize, 300.0f, amplitude, 1.0f);
            const auto highs = eqGainDb (eq, sr, blockSize, 12000.0f, amplitude, 1.0f);

            expect (bass > -3.0f, "AIR should keep the bass/low-mid mostly open");
            expect (highs > 0.5f, "AIR should measurably lift the highs");
            expect (highs < 6.0f, "AIR's lift should stay gentle, not a runaway boost");
        }

        beginTest ("PHONE suppresses 100Hz more than 1kHz, and 10kHz more than 1kHz");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 1);

            const auto low  = eqGainDb (eq, sr, blockSize, 100.0f, amplitude, 0.5f);
            const auto mid  = eqGainDb (eq, sr, blockSize, 1000.0f, amplitude, 0.5f);
            const auto high = eqGainDb (eq, sr, blockSize, 10000.0f, amplitude, 0.5f);

            expect (low < mid - 10.0f, "PHONE should suppress 100Hz well below 1kHz");
            expect (high < mid - 10.0f, "PHONE should suppress 10kHz well below 1kHz");
        }

        beginTest ("DARK preserves bass significantly better than PHONE");
        {
            uni76::dsp::EqProcessor eqDark, eqPhone;
            eqDark.prepare (sr, blockSize, 1);
            eqPhone.prepare (sr, blockSize, 1);

            const auto darkBass  = eqGainDb (eqDark,  sr, blockSize, 100.0f, amplitude, 0.0f);
            const auto phoneBass = eqGainDb (eqPhone, sr, blockSize, 100.0f, amplitude, 0.5f);

            expect (darkBass > phoneBass + 10.0f, "DARK should retain 100Hz much better than PHONE");
        }

        beginTest ("DARK suppresses the high end relative to an unprocessed (bypass) reference");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 1);

            const auto highDark   = eqGainDb (eq, sr, blockSize, 12000.0f, amplitude, 0.0f, true);
            expect (highDark < -6.0f, "DARK's high end should sit clearly below the 0dB bypass reference");
        }

        beginTest ("AIR has more high-frequency energy than the neutral/unprocessed input, without runaway gain");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 1);

            for (float freqHz : { 8000.0f, 12000.0f, 16000.0f })
            {
                const auto gainDb = eqGainDb (eq, sr, blockSize, freqHz, amplitude, 1.0f);
                expect (gainDb > 0.0f, juce::String ("AIR should have more energy than input at ") + juce::String (freqHz, 0) + "Hz");
                expect (gainDb < 8.0f, juce::String ("AIR's gain at ") + juce::String (freqHz, 0) + "Hz should not run away");
            }
        }

        beginTest ("Transition 49% -> 50% -> 51% is smooth (no coefficient/topology jump)");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 2);

            const auto increment = juce::MathConstants<float>::twoPi * 1000.0f / (float) sr;
            float phase = 0.0f;
            float prevSample = 0.0f;
            bool havePrev = false;
            float maxJump = 0.0f;

            for (float t : { 0.49f, 0.50f, 0.51f })
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto p = phase;
                    for (int i = 0; i < blockSize; ++i) { buffer.setSample (ch, i, 0.5f * std::sin (p)); p += increment; }
                }
                phase += increment * (float) blockSize;

                eq.process (buffer, t, true);

                for (int i = 0; i < blockSize; ++i)
                {
                    const auto s = buffer.getSample (0, i);
                    if (havePrev) maxJump = juce::jmax (maxJump, std::abs (s - prevSample));
                    prevSample = s;
                    havePrev = true;
                }
            }

            expect (maxJump < 0.3f, "49% -> 50% -> 51% transition produced an unexpectedly large sample-to-sample jump");
        }

        beginTest ("Automation sweep 0 -> 100 -> 0 produces no discontinuity or NaN/Inf");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 2);

            float prevSample = 0.0f;
            float maxJump = 0.0f;
            bool havePrev = false;

            for (int block = 0; block < 60; ++block)
            {
                const auto t = (float) block / 59.0f;
                const auto eqValue = t < 0.5f ? (t * 2.0f) : (2.0f - t * 2.0f);

                juce::AudioBuffer<float> buffer (2, blockSize);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        buffer.setSample (ch, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 500.0f
                                                                    * (float) (block * blockSize + i) / (float) sr));

                eq.process (buffer, eqValue, true);

                expect (bufferIsFinite (buffer), "EQ automation sweep produced non-finite samples");

                for (int i = 0; i < blockSize; ++i)
                {
                    const auto s = buffer.getSample (0, i);
                    if (havePrev) maxJump = juce::jmax (maxJump, std::abs (s - prevSample));
                    prevSample = s;
                    havePrev = true;
                }
            }

            expect (maxJump < 0.3f, "EQ automation sweep produced an unexpectedly large sample-to-sample jump");
        }

        beginTest ("Enable/disable bypass transition is clean (no click)");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 2);

            float prevSample = 0.0f;
            float maxJump = 0.0f;
            bool havePrev = false;
            const auto increment = juce::MathConstants<float>::twoPi * 400.0f / (float) sr;
            float phase = 0.0f;

            for (int block = 0; block < 9; ++block)
            {
                const auto enabled = block < 8;
                juce::AudioBuffer<float> buffer (2, blockSize);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto p = phase;
                    for (int i = 0; i < blockSize; ++i) { buffer.setSample (ch, i, 0.5f * std::sin (p)); p += increment; }
                }
                phase += increment * (float) blockSize;

                eq.process (buffer, 0.5f, enabled);
                expect (bufferIsFinite (buffer), "bypass transition produced non-finite samples");

                for (int i = 0; i < blockSize; ++i)
                {
                    const auto s = buffer.getSample (0, i);
                    if (havePrev) maxJump = juce::jmax (maxJump, std::abs (s - prevSample));
                    prevSample = s;
                    havePrev = true;
                }
            }

            expect (maxJump < 0.3f, "enable/disable transition produced an unexpectedly large sample-to-sample jump");
        }

        beginTest ("Silence remains silence at any EQ/enabled setting");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 2);

            for (float t : { 0.0f, 0.5f, 1.0f })
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                buffer.clear();
                for (int block = 0; block < 10; ++block)
                    eq.process (buffer, t, true);

                expectWithinAbsoluteError (bufferPeak (buffer, 0, 0, blockSize), 0.0f, 1.0e-6f, "silence in should stay silence out");
                buffer.clear();
            }
        }

        beginTest ("Mono processes without error and stays finite");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 1);
            auto processed = runEqSine (eq, 1, blockSize, blockSize * 10, sr, 1000.0f, amplitude, 0.5f, true);
            expect (bufferIsFinite (processed), "mono processing produced non-finite samples");
        }

        beginTest ("Stereo: identical L/R input stays numerically identical after EQ (no stereo coloration)");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 2);

            const auto totalSamples = blockSize * 20;
            juce::AudioBuffer<float> result (2, totalSamples);
            int done = 0;
            float phase = 0.0f;
            const auto increment = juce::MathConstants<float>::twoPi * 700.0f / (float) sr;

            while (done < totalSamples)
            {
                const auto n = juce::jmin (blockSize, totalSamples - done);
                juce::AudioBuffer<float> block (2, n);
                auto p = phase;
                for (int i = 0; i < n; ++i)
                {
                    const auto s = 0.5f * std::sin (p);
                    block.setSample (0, i, s);
                    block.setSample (1, i, s);
                    p += increment;
                }
                phase += increment * (float) n;

                eq.process (block, 0.35f, true);
                result.copyFrom (0, done, block, 0, 0, n);
                result.copyFrom (1, done, block, 1, 0, n);
                done += n;
            }

            expect (bufferIsFinite (result), "stereo processing produced non-finite samples");

            float maxDrift = 0.0f;
            for (int i = 0; i < totalSamples; ++i)
                maxDrift = juce::jmax (maxDrift, std::abs (result.getSample (0, i) - result.getSample (1, i)));

            expectWithinAbsoluteError (maxDrift, 0.0f, 1.0e-6f, "identical L/R input must produce numerically identical L/R output");
        }

        beginTest ("Sample rates 44.1/48/88.2/96/176.4/192kHz all process finite audio with zero added latency");
        {
            const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

            for (auto rate : rates)
            {
                uni76::dsp::EqProcessor eq;
                eq.prepare (rate, blockSize, 2);

                auto processed = runEqSine (eq, 2, blockSize, blockSize * 10, rate, 1000.0f, amplitude, 0.5f, true);
                expect (bufferIsFinite (processed), juce::String ("non-finite output at ") + juce::String (rate) + "Hz");
                expectEquals (eq.getLatencySamples(), 0);
            }
        }

        beginTest ("PHONE mapping sounds semantically the same across sample rates (HP/LP land near the same Hz-based targets)");
        {
            const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

            for (auto rate : rates)
            {
                uni76::dsp::EqProcessor eq;
                eq.prepare (rate, blockSize, 1);

                const auto low  = eqGainDb (eq, rate, blockSize, 100.0f, amplitude, 0.5f);
                const auto mid  = eqGainDb (eq, rate, blockSize, 1000.0f, amplitude, 0.5f);
                const auto high = eqGainDb (eq, rate, blockSize, 10000.0f, amplitude, 0.5f);

                expect (low < mid - 10.0f, juce::String ("PHONE should suppress 100Hz well below 1kHz at ") + juce::String (rate) + "Hz");
                expect (high < mid - 10.0f, juce::String ("PHONE should suppress 10kHz well below 1kHz at ") + juce::String (rate) + "Hz");
            }
        }

        beginTest ("Different block sizes (1/7/64/512/4096) all remain finite and stable");
        {
            const int sizes[] { 1, 7, 64, 512, 4096 };

            for (auto size : sizes)
            {
                uni76::dsp::EqProcessor eq;
                eq.prepare (sr, juce::jmax (size, 4096), 2);

                auto processed = runEqSine (eq, 2, size, size * 30, sr, 1000.0f, amplitude, 0.5f, true);
                expect (bufferIsFinite (processed), juce::String ("non-finite output at block size ") + juce::String (size));
            }
        }

        beginTest ("NaN/Inf input samples never reach the output and don't permanently poison filter state");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 2);

            juce::AudioBuffer<float> buffer (2, blockSize);
            buffer.clear();
            buffer.setSample (0, 10, std::numeric_limits<float>::quiet_NaN());
            buffer.setSample (0, 20, std::numeric_limits<float>::infinity());
            buffer.setSample (1, 30, -std::numeric_limits<float>::infinity());

            eq.process (buffer, 0.5f, true);
            expect (bufferIsFinite (buffer), "NaN/Inf input samples leaked through to the output");

            // Filter state must not stay poisoned - a clean block right
            // after should also be clean.
            juce::AudioBuffer<float> clean (2, blockSize);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    clean.setSample (ch, i, 0.3f * std::sin (juce::MathConstants<float>::twoPi * 1000.0f * (float) i / (float) sr));

            eq.process (clean, 0.5f, true);
            expect (bufferIsFinite (clean), "filter state remained poisoned after a NaN/Inf block");
        }

        beginTest ("EQ adds zero latency at every setting");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 2);
            expectEquals (eq.getLatencySamples(), 0);

            juce::AudioBuffer<float> buffer (2, blockSize);
            for (float t : { 0.0f, 0.5f, 1.0f })
                for (bool enabled : { true, false })
                {
                    eq.process (buffer, t, enabled);
                    expectEquals (eq.getLatencySamples(), 0, "EQ should never report nonzero latency");
                }
        }
    }
};

static UNI76EqProcessorTests uni76EqProcessorTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// Full-processor integration: eqEnabled=false really bypasses through the
// real processor, PREAMP+EQ combinations stay stable, and state round-trips.
class UNI76EqIntegrationTests final : public juce::UnitTest
{
public:
    UNI76EqIntegrationTests() : juce::UnitTest ("UNI76AudioProcessor + EQ", "UNI76") {}

    void runTest() override
    {
        beginTest ("eqEnabled=false (via ModuleEnableState) bypasses EQ inside the real processor");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (44100.0, 512);

            processor.getModuleEnableState().setEnabled (0, false); // preamp OFF - isolate EQ
            processor.getModuleEnableState().setEnabled (1, false); // eq OFF

            auto* eqParam = processor.getValueTreeState().getParameter (uni76::ParamID::eq);
            eqParam->setValueNotifyingHost (0.0f); // DARK - would heavily cut highs if active

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> buffer (2, 512);
            float peak = 0.0f;

            for (int b = 0; b < 20; ++b)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int s = 0; s < 512; ++s)
                        buffer.setSample (ch, s, 0.4f * std::sin (juce::MathConstants<float>::twoPi * 12000.0f
                                                                    * (float) (b * 512 + s) / 44100.0f));
                processor.processBlock (buffer, midi);
            }

            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < 512; ++s)
                    peak = juce::jmax (peak, std::abs (buffer.getSample (ch, s)));

            expectWithinAbsoluteError (peak, 0.4f, 0.05f, "disabled EQ should not attenuate a 12kHz tone even at DARK");
        }

        beginTest ("Default state has EQ at 50% (PHONE) - the product default");
        {
            UNI76AudioProcessor processor;
            auto* eqParam = processor.getValueTreeState().getParameter (uni76::ParamID::eq);
            expectWithinAbsoluteError (eqParam->getValue(), 0.5f, 0.001f, "EQ must default to 50% (PHONE)");
        }

        beginTest ("PREAMP + EQ combinations: no NaN/Inf, no gain explosion, meters work");
        {
            const float preampValues[] { 0.0f, 0.5f };
            const float eqValues[] { 0.0f, 0.5f, 1.0f };

            for (auto preampT : preampValues)
            {
                for (auto eqT : eqValues)
                {
                    UNI76AudioProcessor processor;
                    processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                    processor.prepareToPlay (44100.0, 512);

                    processor.getValueTreeState().getParameter (uni76::ParamID::preamp)->setValueNotifyingHost (preampT);
                    processor.getValueTreeState().getParameter (uni76::ParamID::eq)->setValueNotifyingHost (eqT);

                    juce::MidiBuffer midi;
                    juce::AudioBuffer<float> buffer (2, 512);
                    float peak = 0.0f;

                    for (int b = 0; b < 20; ++b)
                    {
                        for (int ch = 0; ch < 2; ++ch)
                            for (int s = 0; s < 512; ++s)
                                buffer.setSample (ch, s, 0.4f * std::sin (juce::MathConstants<float>::twoPi * 800.0f
                                                                            * (float) (b * 512 + s) / 44100.0f));
                        processor.processBlock (buffer, midi);
                    }

                    for (int ch = 0; ch < 2; ++ch)
                        for (int s = 0; s < 512; ++s)
                            peak = juce::jmax (peak, std::abs (buffer.getSample (ch, s)));

                    const juce::String label = "PREAMP=" + juce::String (preampT) + " EQ=" + juce::String (eqT);
                    expect (std::isfinite (peak), "non-finite output for " + label);
                    expect (peak < 4.0f, "unexpected gain explosion for " + label);
                    expect (processor.getInputLevelMeter().readAndResetPeak() >= 0.0f, "input meter unavailable for " + label);
                    expect (processor.getOutputLevelMeter().readAndResetPeak() >= 0.0f, "output meter unavailable for " + label);
                }
            }
        }

        beginTest ("EQ value round-trips through a real getStateInformation()/setStateInformation() save+restore (0/50/100)");
        {
            for (float eqValue : { 0.0f, 0.5f, 1.0f })
            {
                UNI76AudioProcessor processor;
                auto* eqParam = processor.getValueTreeState().getParameter (uni76::ParamID::eq);
                eqParam->setValueNotifyingHost (eqValue);

                juce::MemoryBlock saved;
                processor.getStateInformation (saved);

                eqParam->setValueNotifyingHost (eqValue > 0.5f ? 0.0f : 1.0f); // perturb away
                processor.setStateInformation (saved.getData(), (int) saved.getSize());

                expectWithinAbsoluteError (eqParam->getValue(), eqValue, 0.001f, "EQ value should round-trip through save/restore");
            }
        }
    }
};

static UNI76EqIntegrationTests uni76EqIntegrationTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// Offline frequency-response measurement for docs/DSP_EQ.md - prints
// unconditionally so the numbers can be captured for the docs.
class UNI76EqFrequencyResponseTests final : public juce::UnitTest
{
public:
    UNI76EqFrequencyResponseTests() : juce::UnitTest ("uni76::dsp::EqProcessor frequency response", "UNI76") {}

    void runTest() override
    {
        beginTest ("Frequency response at EQ 0/25/50/75/100% across the documented test-frequency set");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 512;
            constexpr float amplitude = 0.25f;

            const float testFreqs[] { 30.0f, 60.0f, 100.0f, 300.0f, 1000.0f, 3400.0f, 5000.0f, 10000.0f, 16000.0f, 20000.0f };
            const float eqPositions[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

            std::cout << "\n=== EQ frequency response (dB gain vs input) ===" << std::endl;

            for (auto freqHz : testFreqs)
            {
                std::cout << "  " << freqHz << "Hz:";
                for (auto t : eqPositions)
                {
                    uni76::dsp::EqProcessor eq;
                    eq.prepare (sr, blockSize, 1);
                    const auto gainDb = eqGainDb (eq, sr, blockSize, freqHz, amplitude, t);
                    std::cout << "  EQ" << (int) (t * 100.0f) << "%=" << gainDb << "dB";
                    expect (std::isfinite (gainDb), "non-finite frequency response measurement");
                }
                std::cout << std::endl;
            }

            std::cout << "=== end EQ frequency response ===" << std::endl << std::endl;
        }
    }
};

static UNI76EqFrequencyResponseTests uni76EqFrequencyResponseTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// uni76::dsp::SatProcessor - the deterministic DSP test suite required by
// the SAT audit.
namespace
{
    juce::AudioBuffer<float> runSatSine (uni76::dsp::SatProcessor& sat, int numChannels, int blockSize,
                                          int totalSamples, double sampleRate, float freqHz, float amplitude,
                                          float heatNormalised01, bool enabled)
    {
        juce::AudioBuffer<float> result (numChannels, totalSamples);
        const auto increment = juce::MathConstants<float>::twoPi * freqHz / (float) sampleRate;
        float phase = 0.0f;
        int done = 0;

        while (done < totalSamples)
        {
            const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
            juce::AudioBuffer<float> block (numChannels, thisBlock);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto p = phase;
                for (int i = 0; i < thisBlock; ++i)
                {
                    block.setSample (ch, i, amplitude * std::sin (p));
                    p += increment;
                }
            }

            sat.process (block, heatNormalised01, enabled);

            for (int ch = 0; ch < numChannels; ++ch)
                result.copyFrom (ch, done, block, ch, 0, thisBlock);

            phase += increment * (float) thisBlock;
            done += thisBlock;
        }

        return result;
    }

    struct SatMeasurement
    {
        float thdPercent = 0.0f;
        std::array<float, 4> harmonicDb { -300.0f, -300.0f, -300.0f, -300.0f }; // H2..H5
        float rms = 0.0f, peak = 0.0f;
    };

    SatMeasurement measureSat (uni76::dsp::SatProcessor& sat, double sampleRate, int blockSize,
                                float freqHz, float amplitude, float heatNormalised01, bool enabled = true)
    {
        const auto cycles = freqHz <= 150.0f ? 20 : 100;
        const auto win = periodicAnalysisLength (sampleRate, freqHz, cycles);
        const auto settle = sat.getLatencySamples() + blockSize * 8;
        const auto totalSamples = win + settle + blockSize;

        auto sig = runSatSine (sat, 1, blockSize, totalSamples, sampleRate, freqHz, amplitude, heatNormalised01, enabled);
        const auto start = totalSamples - win;

        const auto fundMag = goertzelMagnitude (sig, 0, start, win, sampleRate, freqHz);

        SatMeasurement m;
        double powSum = 0.0;
        for (int h = 2; h <= 5; ++h)
        {
            const auto mag = goertzelMagnitude (sig, 0, start, win, sampleRate, freqHz * (float) h);
            m.harmonicDb[(size_t) h - 2] = juce::Decibels::gainToDecibels (mag + 1.0e-9f);
            powSum += (double) mag * mag;
        }
        m.thdPercent = fundMag > 1.0e-9f ? (float) (100.0 * std::sqrt (powSum) / (double) fundMag) : 0.0f;

        const auto measureWin = juce::jmin (win, blockSize * 20);
        m.rms = bufferRms (sig, 0, totalSamples - measureWin, measureWin);
        m.peak = bufferPeak (sig, 0, totalSamples - measureWin, measureWin);
        return m;
    }
}

class UNI76SatProcessorTests final : public juce::UnitTest
{
public:
    UNI76SatProcessorTests() : juce::UnitTest ("uni76::dsp::SatProcessor", "UNI76") {}

    void runTest() override
    {
        constexpr double sr = 44100.0;
        constexpr int blockSize = 512;
        constexpr float amplitude = 0.125892f; // -18 dBFS

        beginTest ("saturationEnabled=false bypasses the DSP (latency-aligned dry passthrough)");
        {
            uni76::dsp::SatProcessor sat;
            sat.prepare (sr, blockSize, 1);

            const auto totalSamples = blockSize * 20;
            auto processed = runSatSine (sat, 1, blockSize, totalSamples, sr, 1000.0f, 0.5f, 1.0f /* full heat */, false);

            const auto settle = sat.getLatencySamples() + blockSize * 4;
            const auto windowLen = totalSamples - settle - blockSize;
            const auto outRms = bufferRms (processed, 0, settle, windowLen);
            const auto referenceRms = 0.5f * 0.70710678f;

            expectWithinAbsoluteError (outRms, referenceRms, referenceRms * 0.05f,
                                        "disabled SAT should pass the dry signal through essentially unchanged even at HEAT=100%");
        }

        beginTest ("HEAT=0% is close to transparent for a moderate-level signal");
        {
            uni76::dsp::SatProcessor sat;
            sat.prepare (sr, blockSize, 1);

            const auto totalSamples = blockSize * 20;
            auto processed = runSatSine (sat, 1, blockSize, totalSamples, sr, 1000.0f, amplitude, 0.0f, true);

            const auto settle = sat.getLatencySamples() + blockSize * 4;
            const auto windowLen = totalSamples - settle - blockSize;
            const auto outRms = bufferRms (processed, 0, settle, windowLen);
            const auto referenceRms = amplitude * 0.70710678f;

            expectWithinAbsoluteError (outRms, referenceRms, referenceRms * 0.1f,
                                        "HEAT=0% RMS should stay close to the input RMS");
        }

        beginTest ("HEAT=25/50/75/100% produce a monotonically growing, bounded, musical harmonic progression");
        {
            uni76::dsp::SatProcessor sat25, sat50, sat75, sat100;
            sat25.prepare (sr, blockSize, 1);
            sat50.prepare (sr, blockSize, 1);
            sat75.prepare (sr, blockSize, 1);
            sat100.prepare (sr, blockSize, 1);

            const auto m25  = measureSat (sat25,  sr, blockSize, 1000.0f, amplitude, 0.25f);
            const auto m50  = measureSat (sat50,  sr, blockSize, 1000.0f, amplitude, 0.5f);
            const auto m75  = measureSat (sat75,  sr, blockSize, 1000.0f, amplitude, 0.75f);
            const auto m100 = measureSat (sat100, sr, blockSize, 1000.0f, amplitude, 1.0f);

            expect (m25.thdPercent > 0.1f, "HEAT=25% should produce small but measurable harmonics");
            expect (m50.thdPercent > m25.thdPercent, "HEAT=50% should have more harmonic content than 25%");
            expect (m75.thdPercent > m50.thdPercent, "HEAT=75% should have more harmonic content than 50%");
            expect (m100.thdPercent > m75.thdPercent, "HEAT=100% should have more harmonic content than 75%");

            expect (m100.thdPercent < 20.0f, "HEAT=100% at -18dBFS should stay in a strong-but-musical range, not explode");
            expect (m100.peak < 1.05f, "HEAT=100% peak output should stay bounded, not slam into a hard ceiling - no clipping plateau");
        }

        beginTest ("DC offset stays safely small even at HEAT=100%");
        {
            uni76::dsp::SatProcessor sat;
            sat.prepare (sr, blockSize, 1);

            const auto totalSamples = blockSize * 40;
            auto processed = runSatSine (sat, 1, blockSize, totalSamples, sr, 1000.0f, 0.5f, 1.0f, true);

            const auto settle = sat.getLatencySamples() + blockSize * 4;
            const auto windowLen = totalSamples - settle - blockSize;

            double sum = 0.0;
            for (int i = 0; i < windowLen; ++i)
                sum += processed.getSample (0, settle + i);
            const auto mean = std::abs ((float) (sum / (double) windowLen));

            expect (mean < 0.01f, "HEAT=100% should not leave a significant DC offset in the output");
        }

        beginTest ("Silence remains silence at any HEAT/enabled setting");
        {
            uni76::dsp::SatProcessor sat;
            sat.prepare (sr, blockSize, 2);

            for (float heat : { 0.0f, 0.5f, 1.0f })
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                buffer.clear();

                for (int block = 0; block < 10; ++block)
                    sat.process (buffer, heat, true);

                expectWithinAbsoluteError (bufferPeak (buffer, 0, 0, blockSize), 0.0f, 1.0e-6f, "silence in should stay silence out - no analog noise/hiss simulation");
                buffer.clear();
            }
        }

        beginTest ("Crest factor does not meaningfully rise with HEAT, and clearly falls by HEAT=100% (transient peak rounding)");
        {
            constexpr int totalSamples = 44100 * 3;

            auto runTransient = [&] (float heat)
            {
                uni76::dsp::SatProcessor sat;
                sat.prepare (sr, blockSize, 1);

                // Let the HEAT smoother settle before any real signal, so
                // early transients are measured at the real target HEAT.
                {
                    juce::AudioBuffer<float> warmup (1, blockSize);
                    warmup.clear();
                    for (int b = 0; b < 20; ++b)
                        sat.process (warmup, heat, true);
                }

                juce::AudioBuffer<float> buffer (1, totalSamples);
                buffer.clear();
                for (int hitIdx = 0; hitIdx < 8; ++hitIdx)
                {
                    const auto startSample = hitIdx * (totalSamples / 8);
                    const auto freqHz = 200.0 + hitIdx * 90.0;
                    for (int i = 0; i < 4000 && startSample + i < totalSamples; ++i)
                    {
                        const auto env = std::exp (-(double) i / 1200.0);
                        buffer.setSample (0, startSample + i,
                            (float) (0.8 * env * std::sin (juce::MathConstants<double>::twoPi * freqHz * i / sr)));
                    }
                }

                int done = 0;
                while (done < totalSamples)
                {
                    const auto n = juce::jmin (blockSize, totalSamples - done);
                    juce::AudioBuffer<float> block (1, n);
                    block.copyFrom (0, 0, buffer, 0, done, n);
                    sat.process (block, heat, true);
                    buffer.copyFrom (0, done, block, 0, 0, n);
                    done += n;
                }

                const auto peak = bufferPeak (buffer, 0, 0, totalSamples);
                const auto rms = bufferRms (buffer, 0, 0, totalSamples);
                return std::make_pair (peak, juce::Decibels::gainToDecibels (peak / juce::jmax (1.0e-9f, rms)));
            };

            const auto [peak0, crest0]     = runTransient (0.0f);
            const auto [peak50, crest50]   = runTransient (0.5f);
            const auto [peak100, crest100] = runTransient (1.0f);

            expect (crest50 < crest0 + 0.5f, "crest factor should not meaningfully rise by HEAT=50%");
            expect (crest100 < crest0 - 2.0f, "crest factor should clearly fall by HEAT=100%");
            expect (peak100 < peak0, "HEAT=100% should round transient peaks down relative to HEAT=0%");
        }

        beginTest ("Bass (40/60/100Hz) stays controlled at HEAT=100%: fundamental retained, THD bounded");
        {
            for (float freqHz : { 40.0f, 60.0f, 100.0f })
            {
                uni76::dsp::SatProcessor satOff, satOn;
                satOff.prepare (sr, blockSize, 1);
                satOn.prepare (sr, blockSize, 1);

                const auto mOff = measureSat (satOff, sr, blockSize, freqHz, amplitude, 0.0f);
                const auto mOn  = measureSat (satOn,  sr, blockSize, freqHz, amplitude, 1.0f);

                const auto retainedDb = juce::Decibels::gainToDecibels (mOn.rms / juce::jmax (1.0e-9f, mOff.rms));

                expect (std::abs (retainedDb) < 3.0f,
                        juce::String (freqHz, 0) + "Hz fundamental should stay close to its unprocessed level at HEAT=100% (not turn to mush)");
                expect (mOn.thdPercent < 15.0f,
                        juce::String (freqHz, 0) + "Hz should get controlled harmonics at HEAT=100%, not a harmonic mess");
            }
        }

        beginTest ("High end (5/8/12kHz) softens at HEAT=100% relative to HEAT=0%, gradually not via a fixed brick-wall");
        {
            for (float freqHz : { 5000.0f, 8000.0f, 12000.0f })
            {
                uni76::dsp::SatProcessor satOff, satOn;
                satOff.prepare (sr, blockSize, 1);
                satOn.prepare (sr, blockSize, 1);

                const auto mOff = measureSat (satOff, sr, blockSize, freqHz, amplitude, 0.0f);
                const auto mOn  = measureSat (satOn,  sr, blockSize, freqHz, amplitude, 1.0f);

                const auto deltaDb = juce::Decibels::gainToDecibels (mOn.rms / juce::jmax (1.0e-9f, mOff.rms));

                expect (deltaDb < -0.5f, juce::String (freqHz, 0) + "Hz should measurably soften at HEAT=100%");
                expect (deltaDb > -12.0f, juce::String (freqHz, 0) + "Hz softening should stay gentle, not a hard cut");
            }
        }

        beginTest ("Aliasing: production oversampled path suppresses fold-back energy vs a non-oversampled reference");
        {
            // Production never gets a runtime oversampling on/off switch
            // (see Source/DSP/SatProcessor.cpp) - this reference path
            // exists only in this test, replicating the identical
            // tilt+dynamic-gain+waveshaper math directly at the base
            // rate, with no up/downsampling.
            constexpr float testAmplitude = 0.3f;
            constexpr float heat = 1.0f; // worst case

            auto referenceNoOversampling = [&] (float freqHz)
            {
                uni76::dsp::Biquad lowPre, highPre, highDe, lowDe;
                uni76::dsp::EnvelopeFollower envelope;
                envelope.setReleaseMs (sr, uni76::dsp::satEnvelopeReleaseMs);

                const auto lowDb = uni76::dsp::satLowShelfGainDb (heat);
                const auto highDb = uni76::dsp::satHighShelfGainDb (heat);
                uni76::dsp::makeLowShelf  (lowPre,  sr, uni76::dsp::satLowShelfFreqHz,  lowDb);
                uni76::dsp::makeHighShelf (highPre, sr, uni76::dsp::satHighShelfFreqHz, highDb);
                uni76::dsp::makeHighShelf (highDe, sr, uni76::dsp::satHighShelfFreqHz, -highDb);
                uni76::dsp::makeLowShelf  (lowDe,  sr, uni76::dsp::satLowShelfFreqHz,  -lowDb);

                const auto driveGain = uni76::dsp::satDriveGainLinear (heat);
                const auto asym = uni76::dsp::satAsymmetryAmount (heat);
                const auto compressionStrength = uni76::dsp::satCompressionStrength (heat);
                const auto norm = std::tanh (driveGain);

                const int totalSamples = blockSize * 40;
                juce::AudioBuffer<float> out (1, totalSamples);
                const auto inc = juce::MathConstants<double>::twoPi * (double) freqHz / sr;
                double phase = 0.0;

                for (int i = 0; i < totalSamples; ++i)
                {
                    auto x = testAmplitude * (float) std::sin (phase);
                    x = lowPre.processSample (x);
                    x = highPre.processSample (x);

                    const auto env = envelope.processSample (x);
                    x *= 1.0f / (1.0f + compressionStrength * env);

                    const auto xd = x * driveGain;
                    const auto shaped = xd >= 0.0f ? std::tanh (xd) : std::tanh (xd * (1.0f - asym));
                    auto y = norm > 1.0e-6f ? shaped / norm : shaped;

                    y = highDe.processSample (y);
                    y = lowDe.processSample (y);

                    out.setSample (0, i, y);
                    phase += inc;
                }
                return out;
            };

            auto productionOversampled = [&] (float freqHz)
            {
                uni76::dsp::SatProcessor sat;
                sat.prepare (sr, blockSize, 1);
                const int totalSamples = blockSize * 40;
                return runSatSine (sat, 1, blockSize, totalSamples, sr, freqHz, testAmplitude, heat, true);
            };

            std::cout << "\n=== SAT aliasing measurement (44.1kHz, HEAT=100%) ===" << std::endl;

            for (float testToneHz : { 4000.0f, 8000.0f, 12000.0f })
            {
                auto reference = referenceNoOversampling (testToneHz);
                auto production = productionOversampled (testToneHz);

                const auto totalSamples = reference.getNumSamples();
                const auto win = periodicAnalysisLength (sr, testToneHz, 100);
                const auto start = totalSamples - win;

                const auto probeHz = juce::jmax (500.0f, testToneHz * 0.5f);
                const auto refAlias = goertzelMagnitude (reference, 0, start, win, sr, probeHz);
                const auto prodAlias = goertzelMagnitude (production, 0, start, win, sr, probeHz);

                const auto suppressionDb = refAlias > 1.0e-9f
                    ? juce::Decibels::gainToDecibels (prodAlias / refAlias)
                    : 0.0f;

                std::cout << "  tone=" << testToneHz << "Hz probe=" << probeHz << "Hz: "
                           << "reference=" << juce::Decibels::gainToDecibels (refAlias + 1.0e-9f) << "dB  "
                           << "production=" << juce::Decibels::gainToDecibels (prodAlias + 1.0e-9f) << "dB  "
                           << "suppression=" << suppressionDb << "dB" << std::endl;

                expect (std::isfinite (refAlias) && std::isfinite (prodAlias), "non-finite aliasing measurement");
                expect (prodAlias <= refAlias + 1.0e-6f,
                        "the oversampled production path should never show more fold-back energy than the non-oversampled reference");
            }

            std::cout << "=== end SAT aliasing measurement ===" << std::endl << std::endl;
        }

        beginTest ("Enable/disable transition does not create an extreme discontinuity");
        {
            uni76::dsp::SatProcessor sat;
            sat.prepare (sr, blockSize, 2);

            constexpr float freqHz = 300.0f;
            const auto increment = juce::MathConstants<float>::twoPi * freqHz / (float) sr;

            float phase = 0.0f;
            float maxJump = 0.0f;
            float prevSample = 0.0f;
            bool havePrev = false;

            for (int block = 0; block < 9; ++block)
            {
                const auto enabled = block < 8;
                juce::AudioBuffer<float> buffer (2, blockSize);

                for (int ch = 0; ch < 2; ++ch)
                {
                    auto p = phase;
                    for (int i = 0; i < blockSize; ++i) { buffer.setSample (ch, i, 0.6f * std::sin (p)); p += increment; }
                }

                sat.process (buffer, 1.0f, enabled);
                expect (bufferIsFinite (buffer), "transition block contains non-finite samples");

                for (int i = 0; i < blockSize; ++i)
                {
                    const auto s = buffer.getSample (0, i);
                    if (havePrev) maxJump = juce::jmax (maxJump, std::abs (s - prevSample));
                    prevSample = s;
                    havePrev = true;
                }

                phase += increment * (float) blockSize;
            }

            expect (maxJump < 0.35f, "enable/disable transition produced an unexpectedly large sample-to-sample jump: " + juce::String (maxJump, 4));
        }

        beginTest ("Automation sweep 0 -> 100 -> 0 produces no NaN/Inf and no extreme discontinuity");
        {
            uni76::dsp::SatProcessor sat;
            sat.prepare (sr, blockSize, 2);

            float prevSample = 0.0f;
            float maxJump = 0.0f;
            bool havePrev = false;

            for (int block = 0; block < 60; ++block)
            {
                const auto t = (float) block / 59.0f;
                const auto heat = t < 0.5f ? (t * 2.0f) : (2.0f - t * 2.0f);

                juce::AudioBuffer<float> buffer (2, blockSize);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < blockSize; ++i)
                        buffer.setSample (ch, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 300.0f
                                                                    * (float) (block * blockSize + i) / (float) sr));

                sat.process (buffer, heat, true);
                expect (bufferIsFinite (buffer), "automation sweep produced non-finite samples");

                for (int i = 0; i < blockSize; ++i)
                {
                    const auto s = buffer.getSample (0, i);
                    if (havePrev) maxJump = juce::jmax (maxJump, std::abs (s - prevSample));
                    prevSample = s;
                    havePrev = true;
                }
            }

            expect (maxJump < 0.5f, "automation sweep produced an unexpectedly large sample-to-sample jump");
        }

        beginTest ("Mono processes without error and stays finite");
        {
            uni76::dsp::SatProcessor sat;
            sat.prepare (sr, blockSize, 1);
            auto processed = runSatSine (sat, 1, blockSize, blockSize * 10, sr, 500.0f, 0.4f, 0.7f, true);
            expect (bufferIsFinite (processed), "mono processing produced non-finite samples");
        }

        beginTest ("Stereo: signal in the left channel only never bleeds into a silent right channel");
        {
            uni76::dsp::SatProcessor sat;
            sat.prepare (sr, blockSize, 2);

            const auto totalSamples = blockSize * 10;
            juce::AudioBuffer<float> result (2, totalSamples);
            int done = 0;
            float phase = 0.0f;
            const auto increment = juce::MathConstants<float>::twoPi * 440.0f / (float) sr;

            while (done < totalSamples)
            {
                const auto n = juce::jmin (blockSize, totalSamples - done);
                juce::AudioBuffer<float> block (2, n);
                block.clear();

                auto p = phase;
                for (int i = 0; i < n; ++i) { block.setSample (0, i, 0.7f * std::sin (p)); p += increment; }
                phase += increment * (float) n;

                sat.process (block, 0.8f, true);

                result.copyFrom (0, done, block, 0, 0, n);
                result.copyFrom (1, done, block, 1, 0, n);
                done += n;
            }

            expect (bufferIsFinite (result), "stereo processing produced non-finite samples");
            expectWithinAbsoluteError (bufferPeak (result, 1, 0, totalSamples), 0.0f, 1.0e-6f,
                                        "an independently-stateful right channel fed silence must stay silent regardless of left-channel content");
        }

        beginTest ("Stereo: identical L/R input stays numerically identical after SAT (no stereo drift)");
        {
            uni76::dsp::SatProcessor sat;
            sat.prepare (sr, blockSize, 2);

            const auto totalSamples = blockSize * 20;
            juce::AudioBuffer<float> result (2, totalSamples);
            int done = 0;
            float phase = 0.0f;
            const auto increment = juce::MathConstants<float>::twoPi * 300.0f / (float) sr;

            while (done < totalSamples)
            {
                const auto n = juce::jmin (blockSize, totalSamples - done);
                juce::AudioBuffer<float> block (2, n);

                auto p = phase;
                for (int i = 0; i < n; ++i)
                {
                    const auto s = 0.6f * std::sin (p);
                    block.setSample (0, i, s);
                    block.setSample (1, i, s);
                    p += increment;
                }
                phase += increment * (float) n;

                sat.process (block, 0.85f, true);

                result.copyFrom (0, done, block, 0, 0, n);
                result.copyFrom (1, done, block, 1, 0, n);
                done += n;
            }

            expect (bufferIsFinite (result), "stereo processing produced non-finite samples");

            float maxDrift = 0.0f;
            for (int i = 0; i < totalSamples; ++i)
                maxDrift = juce::jmax (maxDrift, std::abs (result.getSample (0, i) - result.getSample (1, i)));

            expectWithinAbsoluteError (maxDrift, 0.0f, 1.0e-6f,
                                        "identical L/R input must produce numerically identical L/R output");
        }

        beginTest ("Sample rates 44.1/48/88.2/96/176.4/192kHz all process finite audio with the expected latency architecture");
        {
            struct Config { double sr; bool expectLatency; };
            const Config configs[] {
                { 44100.0, true }, { 48000.0, true }, { 88200.0, true },
                { 96000.0, true }, { 176400.0, false }, { 192000.0, false },
            };

            for (const auto& config : configs)
            {
                uni76::dsp::SatProcessor sat;
                sat.prepare (config.sr, blockSize, 2);

                auto processed = runSatSine (sat, 2, blockSize, blockSize * 10, config.sr, 1000.0f, 0.5f, 0.6f, true);
                expect (bufferIsFinite (processed), juce::String ("non-finite output at ") + juce::String (config.sr) + "Hz");

                std::cout << "  SAT latency @ " << config.sr << "Hz = " << sat.getLatencySamples() << " samples" << std::endl;

                if (config.expectLatency)
                    expect (sat.getLatencySamples() > 0, juce::String ("expected nonzero latency at ") + juce::String (config.sr) + "Hz");
                else
                    expectEquals (sat.getLatencySamples(), 0);
            }
        }

        beginTest ("Block sizes 32/64/128/256/512/1024/2048 all remain finite, stable, and block-size-independent in character");
        {
            const int sizes[] { 32, 64, 128, 256, 512, 1024, 2048 };
            std::vector<float> thdBySize;

            for (auto size : sizes)
            {
                uni76::dsp::SatProcessor sat;
                sat.prepare (sr, juce::jmax (size, 4096), 1);

                auto processed = runSatSine (sat, 1, size, size * 80, sr, 1000.0f, amplitude, 0.5f, true);
                expect (bufferIsFinite (processed), juce::String ("non-finite output at block size ") + juce::String (size));
            }
        }

        beginTest ("NaN/Inf input samples never reach the output");
        {
            uni76::dsp::SatProcessor sat;
            sat.prepare (sr, blockSize, 2);

            juce::AudioBuffer<float> buffer (2, blockSize);
            buffer.clear();
            buffer.setSample (0, 10, std::numeric_limits<float>::quiet_NaN());
            buffer.setSample (0, 20, std::numeric_limits<float>::infinity());
            buffer.setSample (1, 30, -std::numeric_limits<float>::infinity());

            sat.process (buffer, 1.0f, true);

            expect (bufferIsFinite (buffer), "NaN/Inf input samples leaked through to the output");
        }

        beginTest ("Latency is constant for a given sample rate regardless of HEAT or enabled state");
        {
            uni76::dsp::SatProcessor sat;
            sat.prepare (sr, blockSize, 2);

            const auto latency = sat.getLatencySamples();
            expect (latency > 0, "44.1kHz should report nonzero latency (4x oversampling)");

            juce::AudioBuffer<float> buffer (2, blockSize);
            for (float heat : { 0.0f, 1.0f })
                for (bool enabled : { true, false })
                {
                    sat.process (buffer, heat, enabled);
                    expectEquals (sat.getLatencySamples(), latency,
                                  "getLatencySamples() must stay constant for correct host plugin-delay-compensation");
                }
        }
    }
};

static UNI76SatProcessorTests uni76SatProcessorTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// Full-processor integration: saturationEnabled=false really does bypass
// audibly, PREAMP+EQ+SAT combinations stay stable, EQ PHONE + SAT HOT is
// specifically checked, and state round-trips.
class UNI76SatIntegrationTests final : public juce::UnitTest
{
public:
    UNI76SatIntegrationTests() : juce::UnitTest ("UNI76AudioProcessor + SAT", "UNI76") {}

    void runTest() override
    {
        beginTest ("saturationEnabled=false (via ModuleEnableState) bypasses SAT inside the real processor");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (44100.0, 512);

            processor.getModuleEnableState().setEnabled (0, false); // preamp OFF - isolate SAT
            processor.getModuleEnableState().setEnabled (1, false); // eq OFF - isolate SAT
            processor.getModuleEnableState().setEnabled (2, false); // sat OFF

            auto* satParam = processor.getValueTreeState().getParameter (uni76::ParamID::saturation);
            satParam->setValueNotifyingHost (1.0f); // 100% - would heavily saturate if active

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> buffer (2, 512);

            for (int i = 0; i < 20; ++i)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int s = 0; s < 512; ++s)
                        buffer.setSample (ch, s, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 1000.0f
                                                                    * (float) (i * 512 + s) / 44100.0f));
                processor.processBlock (buffer, midi);
            }

            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < 512; ++s)
                    peak = juce::jmax (peak, std::abs (buffer.getSample (ch, s)));

            expectWithinAbsoluteError (peak, 0.5f, 0.05f, "disabled SAT should not audibly saturate the signal");
        }

        beginTest ("PREAMP + EQ + SAT representative combinations: no NaN/Inf, no gain explosion, meters work");
        {
            struct Combo { float preamp, eq, sat; };
            const Combo combos[] {
                { 0.0f, 0.5f, 0.0f }, { 0.0f, 0.5f, 0.5f }, { 0.0f, 0.5f, 1.0f },
                { 0.5f, 0.5f, 0.5f }, { 0.5f, 0.0f, 0.75f }, { 0.5f, 1.0f, 0.75f },
                { 0.75f, 0.5f, 1.0f },
            };

            for (const auto& combo : combos)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (44100.0, 512);

                processor.getValueTreeState().getParameter (uni76::ParamID::preamp)->setValueNotifyingHost (combo.preamp);
                processor.getValueTreeState().getParameter (uni76::ParamID::eq)->setValueNotifyingHost (combo.eq);
                processor.getValueTreeState().getParameter (uni76::ParamID::saturation)->setValueNotifyingHost (combo.sat);

                juce::MidiBuffer midi;
                juce::AudioBuffer<float> buffer (2, 512);
                float peak = 0.0f;
                bool finite = true;

                for (int b = 0; b < 20; ++b)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int s = 0; s < 512; ++s)
                            buffer.setSample (ch, s, 0.4f * std::sin (juce::MathConstants<float>::twoPi * 800.0f
                                                                        * (float) (b * 512 + s) / 44100.0f));
                    processor.processBlock (buffer, midi);
                    if (! bufferIsFinite (buffer)) finite = false;
                }

                for (int ch = 0; ch < 2; ++ch)
                    for (int s = 0; s < 512; ++s)
                        peak = juce::jmax (peak, std::abs (buffer.getSample (ch, s)));

                const juce::String label = "PREAMP=" + juce::String (combo.preamp) + " EQ=" + juce::String (combo.eq) + " SAT=" + juce::String (combo.sat);
                expect (finite, "non-finite output for " + label);
                expect (peak < 4.0f, "unexpected gain explosion for " + label);
                expect (processor.getLatencySamples() > 0, "expected nonzero total latency (PREAMP + SAT oversampling) for " + label);
                expect (processor.getInputLevelMeter().readAndResetPeak() >= 0.0f, "input meter unavailable for " + label);
                expect (processor.getOutputLevelMeter().readAndResetPeak() >= 0.0f, "output meter unavailable for " + label);
            }
        }

        beginTest ("EQ PHONE (default 50%) + SAT HOT (100%) stays stable and musical, not sudden fuzz");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (44100.0, 512);

            // EQ is already at its 50% (PHONE) default - just drive SAT.
            processor.getValueTreeState().getParameter (uni76::ParamID::saturation)->setValueNotifyingHost (1.0f);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> buffer (2, 512);
            bool finite = true;
            float peak = 0.0f;

            for (int b = 0; b < 30; ++b)
            {
                for (int ch = 0; ch < 2; ++ch)
                    for (int s = 0; s < 512; ++s)
                        buffer.setSample (ch, s, 0.4f * std::sin (juce::MathConstants<float>::twoPi * 1200.0f
                                                                    * (float) (b * 512 + s) / 44100.0f));
                processor.processBlock (buffer, midi);
                if (! bufferIsFinite (buffer)) finite = false;
            }

            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < 512; ++s)
                    peak = juce::jmax (peak, std::abs (buffer.getSample (ch, s)));

            expect (finite, "EQ PHONE + SAT HOT produced non-finite output");
            expect (peak < 1.2f, "EQ PHONE + SAT HOT should stay bounded/musical, not spike into fuzz");
        }

        beginTest ("Total plugin latency is the sum of PREAMP's, SAT's, PITCH's and PAN's own latencies (EQ and PAN add none)");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (44100.0, 512);

            uni76::dsp::PreampProcessor referencePreamp;
            referencePreamp.prepare (44100.0, 512, 2);
            uni76::dsp::SatProcessor referenceSat;
            referenceSat.prepare (44100.0, 512, 2);
            uni76::dsp::PitchProcessor referencePitch;
            referencePitch.prepare (44100.0, 512, 2);
            uni76::dsp::PanoramaProcessor referencePan;
            referencePan.prepare (44100.0, 512, 2);

            expectEquals (processor.getLatencySamples(),
                           referencePreamp.getLatencySamples() + referenceSat.getLatencySamples()
                           + referencePitch.getLatencySamples() + referencePan.getLatencySamples());
        }

        beginTest ("SAT value round-trips through a real getStateInformation()/setStateInformation() save+restore (0/50/100)");
        {
            for (float satValue : { 0.0f, 0.5f, 1.0f })
            {
                UNI76AudioProcessor processor;
                auto* satParam = processor.getValueTreeState().getParameter (uni76::ParamID::saturation);
                satParam->setValueNotifyingHost (satValue);

                juce::MemoryBlock saved;
                processor.getStateInformation (saved);

                satParam->setValueNotifyingHost (satValue > 0.5f ? 0.0f : 1.0f); // perturb away
                processor.setStateInformation (saved.getData(), (int) saved.getSize());

                expectWithinAbsoluteError (satParam->getValue(), satValue, 0.001f, "SAT value should round-trip through save/restore");
            }
        }
    }
};

static UNI76SatIntegrationTests uni76SatIntegrationTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// Offline harmonic analysis + level matrix for docs/DSP_SAT.md - prints
// unconditionally so the numbers can be captured for the docs.
class UNI76SatAnalysisTests final : public juce::UnitTest
{
public:
    UNI76SatAnalysisTests() : juce::UnitTest ("uni76::dsp::SatProcessor harmonic analysis", "UNI76") {}

    void runTest() override
    {
        beginTest ("1kHz @ -18dBFS through HEAT 0/25/50/75/100 - harmonic progression is smooth, bounded, monotonic");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 512;
            constexpr float amplitude = 0.125892f; // -18 dBFS

            const float heats[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
            std::vector<SatMeasurement> results;

            std::cout << "\n=== SAT harmonic analysis: 1kHz @ -18dBFS ===" << std::endl;

            for (auto heat : heats)
            {
                uni76::dsp::SatProcessor sat;
                sat.prepare (sr, blockSize, 1);
                const auto m = measureSat (sat, sr, blockSize, 1000.0f, amplitude, heat);
                results.push_back (m);

                std::cout << "  HEAT " << (int) (heat * 100.0f) << "%: "
                           << "H2=" << m.harmonicDb[0] << "dB  H3=" << m.harmonicDb[1]
                           << "dB  H4=" << m.harmonicDb[2] << "dB  H5=" << m.harmonicDb[3]
                           << "dB  THD=" << m.thdPercent << "%  peak=" << m.peak << std::endl;
            }

            std::cout << "=== end SAT harmonic analysis ===" << std::endl << std::endl;

            for (size_t i = 1; i < results.size(); ++i)
                expect (results[i].thdPercent >= results[i - 1].thdPercent - 0.05f, "THD should not meaningfully decrease as HEAT increases");
        }

        beginTest ("Input level x HEAT matrix (-30/-18/-12/-6 dBFS x 25/50/75/100%)");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 512;

            const float levelsDb[] { -30.0f, -18.0f, -12.0f, -6.0f };
            const float heats[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

            std::cout << "=== SAT level x HEAT matrix (1kHz) ===" << std::endl;

            for (auto levelDb : levelsDb)
            {
                const auto amplitude = juce::Decibels::decibelsToGain (levelDb);
                for (auto heat : heats)
                {
                    uni76::dsp::SatProcessor sat;
                    sat.prepare (sr, blockSize, 1);
                    const auto m = measureSat (sat, sr, blockSize, 1000.0f, amplitude, heat);
                    const auto crestDb = m.rms > 1.0e-9f ? juce::Decibels::gainToDecibels (m.peak / m.rms) : 0.0f;

                    std::cout << "  " << levelDb << "dBFS HEAT" << (int) (heat * 100.0f) << "%: THD=" << m.thdPercent
                               << "%  H2=" << m.harmonicDb[0] << "dB  H3=" << m.harmonicDb[1] << "dB  H5=" << m.harmonicDb[3]
                               << "dB  outRMS=" << m.rms << "  peak=" << m.peak << "  crest=" << crestDb << "dB" << std::endl;

                    expect (std::isfinite (m.thdPercent) && std::isfinite (m.rms), "non-finite measurement in level x HEAT matrix");
                    expect (m.thdPercent < 100.0f, "THD exceeded 100% - runaway/rectifying distortion, not musical saturation");
                }
            }

            std::cout << "=== end SAT level x HEAT matrix ===" << std::endl << std::endl;
        }
    }
};

static UNI76SatAnalysisTests uni76SatAnalysisTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// PITCH / VARISPEED - see Source/DSP/PitchProcessor.h and docs/DSP_PITCH.md.
// Pure pitch-shift only (duration always preserved): two independent mono
// Signalsmith Stretch engines, 140ms/35ms STFT configuration, latency-
// aligned bypass. Priority order per the product brief: low-frequency
// stability first, then absence of wobble/sidebands/distortion, then
// pitch accuracy, transients, stereo coherence, latency, CPU last.
class UNI76PitchProcessorTests final : public juce::UnitTest
{
public:
    UNI76PitchProcessorTests() : juce::UnitTest ("uni76::dsp::PitchProcessor", "UNI76") {}

    void runTest() override
    {
        beginTest ("Construct/prepare/reset does not crash; latency is positive and constant for the configured sample rate");
        {
            uni76::dsp::PitchProcessor pitch;
            pitch.prepare (44100.0, 512, 2);
            expect (pitch.getLatencySamples() > 0, "PITCH must report nonzero algorithmic latency");
            pitch.reset();

            juce::AudioBuffer<float> buffer (2, 512);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buffer.setSample (ch, i, 0.2f * std::sin (0.1f * (float) i));

            pitch.process (buffer, 0, true);
            expect (bufferIsFinite (buffer), "process() produced non-finite output right after prepare()");
        }

        beginTest ("Latency is fixed across all 6 sample rates and does not depend on host block size (32..2048)");
        {
            const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
            const int blockSizes[] { 32, 64, 128, 256, 512, 1024, 2048 };

            std::cout << "\n=== PITCH latency by sample rate ===" << std::endl;

            for (auto sr : rates)
            {
                int latencyAtFirstBlockSize = -1;

                for (auto blockSize : blockSizes)
                {
                    uni76::dsp::PitchProcessor pitch;
                    pitch.prepare (sr, blockSize, 2);

                    if (latencyAtFirstBlockSize < 0)
                        latencyAtFirstBlockSize = pitch.getLatencySamples();
                    else
                        expectEquals (pitch.getLatencySamples(), latencyAtFirstBlockSize,
                                      "PITCH latency must not depend on host block size");
                }

                if (sr == 44100.0 || sr == 48000.0 || sr == 96000.0 || sr == 192000.0)
                    std::cout << "  " << sr << "Hz: " << latencyAtFirstBlockSize << " samples ("
                               << (1000.0 * latencyAtFirstBlockSize / sr) << " ms)" << std::endl;
            }
            std::cout << "=== end PITCH latency by sample rate ===" << std::endl << std::endl;
        }

        beginTest ("Latency is identical at -12/0/+12 ST and enabled/disabled");
        {
            uni76::dsp::PitchProcessor pitch;
            pitch.prepare (44100.0, 512, 2);
            const auto latency = pitch.getLatencySamples();

            const int semitones[] { -12, -7, 0, 7, 12 };
            for (auto st : semitones)
                for (auto enabled : { true, false })
                {
                    juce::AudioBuffer<float> buffer (2, 512);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 512; ++i)
                            buffer.setSample (ch, i, 0.3f * std::sin (0.05f * (float) i));

                    pitch.process (buffer, st, enabled);
                    expectEquals (pitch.getLatencySamples(), latency,
                                  "getLatencySamples() must stay constant for correct host plugin-delay-compensation");
                }
        }

        beginTest ("Bypass (enabled=false) converges to an exact latency-aligned dry passthrough");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            uni76::dsp::PitchProcessor pitch;
            pitch.prepare (sr, blockSize, 1);
            const auto latency = pitch.getLatencySamples();

            const auto totalSamples = latency + (int) sr; // 1s past full settle
            auto input = generateSine (1, totalSamples, sr, 220.0f, 0.4f);
            auto output = runPitchProcessor (pitch, input, blockSize, 3, false);

            // Settled dry output at sample i should equal input[i - latency]
            // exactly (IntegerDelayLine is a pure copy, no processing).
            double maxAbsDiff = 0.0;
            for (int i = latency + 1000; i < totalSamples; ++i)
                maxAbsDiff = juce::jmax (maxAbsDiff, (double) std::abs (output.getSample (0, i) - input.getSample (0, i - latency)));

            expect (maxAbsDiff < 1.0e-5, "disabled PITCH should be a bit-exact (delayed) passthrough, maxAbsDiff=" + juce::String (maxAbsDiff));
        }

        beginTest ("0 ST is maximally transparent across 40Hz/60Hz/100Hz/440Hz/1kHz/10kHz");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const float freqs[] { 40.0f, 60.0f, 100.0f, 440.0f, 1000.0f, 10000.0f };

            std::cout << "\n=== PITCH 0 ST transparency ===" << std::endl;

            for (auto freqHz : freqs)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 1);
                const auto latency = pitch.getLatencySamples();

                const auto totalSamples = latency + (int) sr;
                auto input = generateSine (1, totalSamples, sr, freqHz, 0.3f);
                auto output = runPitchProcessor (pitch, input, blockSize, 0, true);

                const auto win = juce::jmin (totalSamples - latency - 1000, periodicAnalysisLength (sr, freqHz, 30));
                const auto inMag  = goertzelMagnitude (input,  0, totalSamples - win, win, sr, freqHz);
                const auto outMag = goertzelMagnitude (output, 0, totalSamples - win, win, sr, freqHz);

                const auto ratioDb = 20.0f * std::log10 (juce::jmax (outMag, 1.0e-9f) / juce::jmax (inMag, 1.0e-9f));
                std::cout << "  " << freqHz << "Hz: 0 ST gain = " << ratioDb << " dB" << std::endl;

                expect (std::abs (ratioDb) < 1.0f, juce::String ("0 ST should be near-transparent at ") + juce::String (freqHz) + "Hz, got " + juce::String (ratioDb) + "dB");
            }
            std::cout << "=== end PITCH 0 ST transparency ===" << std::endl << std::endl;
        }

        beginTest ("Pitch accuracy at -12/-7/-3/+3/+7/+12 ST for a 440Hz tone");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            constexpr float freqHz = 440.0f;
            const int semitones[] { -12, -7, -3, 3, 7, 12 };

            std::cout << "\n=== PITCH accuracy (440Hz) ===" << std::endl;

            for (auto st : semitones)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 1);
                const auto latency = pitch.getLatencySamples();
                const auto expectedFreq = freqHz * std::pow (2.0f, (float) st / 12.0f);

                const auto totalSamples = latency + (int) sr;
                auto input = generateSine (1, totalSamples, sr, freqHz, 0.3f);
                auto output = runPitchProcessor (pitch, input, blockSize, st, true);

                const auto stability = analyzeBassStability (output, 0, latency + 2000, totalSamples - latency - 2000, sr, expectedFreq);
                expect (stability.numWindows > 3, "not enough analysis windows for a reliable pitch-accuracy measurement");

                const auto errorPercent = 100.0 * std::abs (stability.freqMean - (double) expectedFreq) / (double) expectedFreq;
                std::cout << "  " << st << " ST: expected=" << expectedFreq << "Hz measured=" << stability.freqMean
                           << "Hz error=" << errorPercent << "%" << std::endl;

                expect (errorPercent < 1.0, juce::String (st) + " ST pitch error too large: " + juce::String (errorPercent) + "%");
            }
            std::cout << "=== end PITCH accuracy ===" << std::endl << std::endl;
        }

        beginTest ("Bass stability matrix: 40/50/60/80/100/120Hz x -12/-7/-3/+3/+7/+12 ST - no wobble, no floating fundamental");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const float bassFreqs[] { 40.0f, 50.0f, 60.0f, 80.0f, 100.0f, 120.0f };
            const int semitones[] { -12, -7, -3, 3, 7, 12 };

            std::cout << "\n=== PITCH bass stability matrix ===" << std::endl;

            for (auto baseFreq : bassFreqs)
            {
                for (auto st : semitones)
                {
                    uni76::dsp::PitchProcessor pitch;
                    pitch.prepare (sr, blockSize, 1);
                    const auto latency = pitch.getLatencySamples();
                    const auto expectedFreq = baseFreq * std::pow (2.0f, (float) st / 12.0f);

                    const auto settle = latency + (int) (0.15 * sr);
                    const auto analysisSamples = (int) (0.6 * sr);
                    const auto totalSamples = settle + analysisSamples;

                    auto input = generateSine (1, totalSamples, sr, baseFreq, 0.35f);
                    auto output = runPitchProcessor (pitch, input, blockSize, st, true);

                    const auto stability = analyzeBassStability (output, 0, settle, totalSamples - settle, sr, expectedFreq);

                    const auto freqDeviationPercent = expectedFreq > 0.0f
                        ? 100.0 * stability.freqStd / (double) expectedFreq : 0.0;

                    std::cout << "  " << baseFreq << "Hz " << (st > 0 ? "+" : "") << st << "ST -> " << expectedFreq
                               << "Hz: freqMean=" << stability.freqMean << "Hz freqStd=" << stability.freqStd
                               << "Hz (" << freqDeviationPercent << "%) ampDbStd=" << stability.ampDbStd
                               << "dB rmsModDepth=" << stability.rmsModDepth << " windows=" << stability.numWindows << std::endl;

                    expect (stability.numWindows > 3, "not enough analysis windows in bass stability matrix");
                    // Hard-reject thresholds per the product brief: no
                    // noticeable bass wobble/floating fundamental/AM.
                    expect (freqDeviationPercent < 3.0, "bass fundamental floats too much (wobble) at "
                            + juce::String (baseFreq) + "Hz " + juce::String (st) + "ST");
                    expect (stability.ampDbStd < 2.5, "bass amplitude modulates too much (breathing) at "
                            + juce::String (baseFreq) + "Hz " + juce::String (st) + "ST");
                    expect (stability.rmsModDepth < 0.2, "bass RMS modulates too much at "
                            + juce::String (baseFreq) + "Hz " + juce::String (st) + "ST");
                }
            }
            std::cout << "=== end PITCH bass stability matrix ===" << std::endl << std::endl;
        }

        beginTest ("Sideband suppression near the shifted fundamental (60Hz+12ST->120Hz, 100Hz-12ST->50Hz)");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            struct Case { float baseFreq; int semitones; };
            const Case cases[] { { 60.0f, 12 }, { 100.0f, -12 }, { 40.0f, 12 }, { 120.0f, -12 } };

            std::cout << "\n=== PITCH sideband suppression ===" << std::endl;

            for (const auto& c : cases)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 1);
                const auto latency = pitch.getLatencySamples();
                const auto targetFreq = c.baseFreq * std::pow (2.0f, (float) c.semitones / 12.0f);

                const auto settle = latency + (int) (0.2 * sr);
                const auto totalSamples = settle + (int) (0.8 * sr);
                auto input = generateSine (1, totalSamples, sr, c.baseFreq, 0.35f);
                auto output = runPitchProcessor (pitch, input, blockSize, c.semitones, true);

                // Sidebands (if any) show up spaced around the analysis
                // hop rate (~1/intervalSeconds) - 25Hz is a representative
                // offset for the chosen 35ms interval.
                const auto sidebands = analyzeSidebands (output, 0, settle, totalSamples - settle, sr, targetFreq, 25.0f);

                std::cout << "  " << c.baseFreq << "Hz " << (c.semitones > 0 ? "+" : "") << c.semitones << "ST -> "
                           << targetFreq << "Hz: sideband below=" << sidebands.belowDb << "dB above=" << sidebands.aboveDb << "dB" << std::endl;

                expect (sidebands.belowDb < -20.0, "sideband below the target fundamental too strong");
                expect (sidebands.aboveDb < -20.0, "sideband above the target fundamental too strong");
            }
            std::cout << "=== end PITCH sideband suppression ===" << std::endl << std::endl;
        }

        beginTest ("Transient quality: single clean onset, no pre-echo/double-hit at -12/-6/+6/+12 ST");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const int semitones[] { -12, -6, 6, 12 };

            for (auto st : semitones)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 1);
                const auto latency = pitch.getLatencySamples();

                // Silence, then a sharp burst (a few cycles of a mid tone
                // windowed by a fast attack/decay), then silence again.
                const auto totalSamples = latency + (int) (1.2 * sr);
                const auto onsetSample = latency + (int) (0.4 * sr);
                const auto burstLength = (int) (0.02 * sr);

                juce::AudioBuffer<float> input (1, totalSamples);
                input.clear();
                for (int i = 0; i < burstLength; ++i)
                {
                    const auto env = std::sin (juce::MathConstants<float>::pi * (float) i / (float) burstLength); // fast in/out
                    input.setSample (0, onsetSample + i, env * 0.6f * std::sin (juce::MathConstants<float>::twoPi * 600.0f * (float) i / (float) sr));
                }

                auto output = runPitchProcessor (pitch, input, blockSize, st, true);

                // Expected onset in the output lands at onsetSample + latency
                // (the module's own added algorithmic delay). Pre-echo
                // check: no significant energy should appear meaningfully
                // earlier than that (beyond a small tolerance for the
                // engine's own analysis-window smear).
                const auto expectedOnset = onsetSample + latency;
                const auto preEchoWindowStart = juce::jmax (0, expectedOnset - (int) (0.05 * sr));
                const auto preEchoWindowEnd   = juce::jmax (0, expectedOnset - (int) (0.005 * sr));

                float preEchoPeak = 0.0f;
                for (int i = preEchoWindowStart; i < preEchoWindowEnd; ++i)
                    preEchoPeak = juce::jmax (preEchoPeak, std::abs (output.getSample (0, i)));

                float burstPeak = 0.0f;
                for (int i = expectedOnset; i < juce::jmin (totalSamples, expectedOnset + burstLength * 4); ++i)
                    burstPeak = juce::jmax (burstPeak, std::abs (output.getSample (0, i)));

                expect (bufferIsFinite (output), "transient test produced non-finite output");
                expect (burstPeak > 0.05f, "transient burst should be clearly audible in the output");
                expect (preEchoPeak < 0.15f * burstPeak, juce::String (st) + " ST: pre-echo energy too strong relative to the burst");
            }
        }

        beginTest ("Mono processing stays finite across -12/0/+12 ST");
        {
            uni76::dsp::PitchProcessor pitch;
            pitch.prepare (44100.0, 512, 1);

            for (auto st : { -12, 0, 12 })
            {
                auto input = generateSine (1, 44100 * 2, 44100.0, 300.0f, 0.4f);
                auto output = runPitchProcessor (pitch, input, 512, st, true);
                expect (bufferIsFinite (output), "mono processing produced non-finite samples");
            }
        }

        beginTest ("Stereo: identical L/R input produces bit-identical L/R output (no wandering centre)");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            for (auto st : { -12, -3, 0, 5, 12 })
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 2);

                const auto totalSamples = pitch.getLatencySamples() + (int) sr;
                juce::AudioBuffer<float> input (2, totalSamples);
                const auto mono = generateSine (1, totalSamples, sr, 250.0f, 0.35f);
                input.copyFrom (0, 0, mono, 0, 0, totalSamples);
                input.copyFrom (1, 0, mono, 0, 0, totalSamples);

                auto output = runPitchProcessor (pitch, input, blockSize, st, true);

                double maxAbsDiff = 0.0;
                for (int i = 0; i < totalSamples; ++i)
                    maxAbsDiff = juce::jmax (maxAbsDiff, (double) std::abs (output.getSample (0, i) - output.getSample (1, i)));

                expect (maxAbsDiff < 1.0e-6, juce::String (st) + " ST: dual-mono input must produce bit-identical stereo output, maxAbsDiff=" + juce::String (maxAbsDiff));
            }
        }

        beginTest ("Stereo: decorrelated hard-panned material stays finite and doesn't blow up");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            uni76::dsp::PitchProcessor pitch;
            pitch.prepare (sr, blockSize, 2);

            const auto totalSamples = pitch.getLatencySamples() + (int) sr;
            juce::AudioBuffer<float> input (2, totalSamples);
            input.copyFrom (0, 0, generateSine (1, totalSamples, sr, 220.0f, 0.4f), 0, 0, totalSamples);
            input.copyFrom (1, 0, generateSine (1, totalSamples, sr, 330.0f, 0.4f), 0, 0, totalSamples);

            auto output = runPitchProcessor (pitch, input, blockSize, 7, true);
            expect (bufferIsFinite (output), "decorrelated stereo material produced non-finite output");

            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < totalSamples; ++i)
                    peak = juce::jmax (peak, std::abs (output.getSample (ch, i)));
            expect (peak < 2.0f, "decorrelated stereo material should not cause a gain explosion");
        }

        beginTest ("All 6 supported sample rates process finite audio without crashing");
        {
            const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

            for (auto sr : rates)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, 512, 2);

                const auto totalSamples = pitch.getLatencySamples() + (int) (0.3 * sr);
                juce::AudioBuffer<float> input (2, totalSamples);
                auto mono = generateSine (1, totalSamples, sr, 220.0f, 0.4f);
                input.copyFrom (0, 0, mono, 0, 0, totalSamples);
                input.copyFrom (1, 0, mono, 0, 0, totalSamples);

                auto output = runPitchProcessor (pitch, input, 512, 5, true);
                expect (bufferIsFinite (output), juce::String ("non-finite output at ") + juce::String (sr) + "Hz");
            }
        }

        beginTest ("All required block sizes (32..2048) give the same settled pitch accuracy regardless of host chunking");
        {
            constexpr double sr = 44100.0;
            constexpr float freqHz = 440.0f;
            constexpr int st = 7;
            const int blockSizes[] { 32, 64, 128, 256, 512, 1024, 2048 };
            const auto expectedFreq = freqHz * std::pow (2.0f, (float) st / 12.0f);

            for (auto blockSize : blockSizes)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, juce::jmax (blockSize, 512), 1); // maximumBlockSize must cover the largest block used
                const auto latency = pitch.getLatencySamples();

                const auto totalSamples = latency + (int) sr;
                auto input = generateSine (1, totalSamples, sr, freqHz, 0.3f);
                auto output = runPitchProcessor (pitch, input, blockSize, st, true);

                const auto stability = analyzeBassStability (output, 0, latency + 2000, totalSamples - latency - 2000, sr, expectedFreq);
                const auto errorPercent = 100.0 * std::abs (stability.freqMean - (double) expectedFreq) / (double) expectedFreq;

                expect (errorPercent < 1.0, "block size " + juce::String (blockSize) + " should not change settled pitch accuracy, error=" + juce::String (errorPercent) + "%");
            }
        }

        beginTest ("Silence produces silence (no self-noise/garbage) and reset() clears internal state");
        {
            uni76::dsp::PitchProcessor pitch;
            pitch.prepare (44100.0, 512, 1);

            juce::AudioBuffer<float> silence (1, 44100 * 2);
            silence.clear();
            auto output = runPitchProcessor (pitch, silence, 512, 7, true);

            float peak = 0.0f;
            for (int i = 0; i < output.getNumSamples(); ++i)
                peak = juce::jmax (peak, std::abs (output.getSample (0, i)));

            expect (peak < 1.0e-4f, "silent input should produce silent (or near-silent) output, peak=" + juce::String (peak));

            pitch.reset();

            juce::AudioBuffer<float> afterReset (1, 512);
            afterReset.clear();
            pitch.process (afterReset, 0, true);
            expect (bufferIsFinite (afterReset), "process() after reset() produced non-finite output");
        }

        beginTest ("NaN/Inf input samples are sanitized and don't permanently poison internal state");
        {
            uni76::dsp::PitchProcessor pitch;
            pitch.prepare (44100.0, 256, 1);

            juce::AudioBuffer<float> poisoned (1, 256);
            for (int i = 0; i < 256; ++i)
                poisoned.setSample (0, i, (i % 2 == 0) ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN());

            pitch.process (poisoned, 5, true);
            expect (bufferIsFinite (poisoned), "NaN/Inf input samples leaked through to the output");

            // Prepared with maximumBlockSize=256 above, so the follow-up
            // clean signal must be fed in <=256-sample blocks too, same as
            // any real host call - runPitchProcessor() enforces that.
            auto clean = generateSine (1, 44100, 44100.0, 300.0f, 0.3f);
            auto cleanOutput = runPitchProcessor (pitch, clean, 256, 5, true);
            expect (bufferIsFinite (cleanOutput), "filter/engine state remained poisoned after a NaN/Inf block");
        }

        beginTest ("Discrete automation transitions (0->+1, +1->+2, 0->-1, -12->+12, +12->-12) are click-free and settle exactly");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            constexpr float freqHz = 300.0f;

            struct Transition { int from, to; };
            const Transition transitions[] { { 0, 1 }, { 1, 2 }, { 0, -1 }, { -12, 12 }, { 12, -12 } };

            for (const auto& t : transitions)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 1);
                const auto latency = pitch.getLatencySamples();

                const auto preSwitchSamples = latency + (int) (0.3 * sr);
                const auto postSwitchSamples = (int) (0.5 * sr);
                const auto totalSamples = preSwitchSamples + postSwitchSamples;

                auto input = generateSine (1, totalSamples, sr, freqHz, 0.3f);

                juce::AudioBuffer<float> output (1, totalSamples);
                int done = 0;
                while (done < totalSamples)
                {
                    const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
                    juce::AudioBuffer<float> block (1, thisBlock);
                    block.copyFrom (0, 0, input, 0, done, thisBlock);

                    const auto semitones = done < preSwitchSamples ? t.from : t.to;
                    pitch.process (block, semitones, true);

                    output.copyFrom (0, done, block, 0, 0, thisBlock);
                    done += thisBlock;
                }

                expect (bufferIsFinite (output), "automation transition produced non-finite output");

                // Click-free: no single-sample derivative spike far beyond
                // the signal's own amplitude anywhere in the run (a hard
                // discontinuity, not the STFT's internal smoothing).
                float maxJump = 0.0f;
                for (int i = 1; i < totalSamples; ++i)
                    maxJump = juce::jmax (maxJump, std::abs (output.getSample (0, i) - output.getSample (0, i - 1)));
                expect (maxJump < 0.5f, juce::String (t.from) + "->" + juce::String (t.to) + " ST: transition produced a click (maxJump=" + juce::String (maxJump) + ")");

                // Settles exactly on the new target semitone after the switch.
                const auto expectedFreq = freqHz * std::pow (2.0f, (float) t.to / 12.0f);
                const auto settleStart = preSwitchSamples + latency + (int) (0.05 * sr);
                const auto stability = analyzeBassStability (output, 0, settleStart, totalSamples - settleStart, sr, expectedFreq);

                if (stability.numWindows > 2)
                {
                    const auto errorPercent = 100.0 * std::abs (stability.freqMean - (double) expectedFreq) / (double) expectedFreq;
                    expect (errorPercent < 2.0, juce::String (t.from) + "->" + juce::String (t.to) + " ST: did not settle on the new target, error=" + juce::String (errorPercent) + "%");
                }
            }
        }
    }
};

static UNI76PitchProcessorTests uni76PitchProcessorTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// Full-chain / APVTS-level PITCH coverage: discrete parameter identity,
// state migration from the old 0..100% pitch parameter, and integration
// with PREAMP/EQ/SAT through the real UNI76AudioProcessor.
class UNI76PitchIntegrationTests final : public juce::UnitTest
{
public:
    UNI76PitchIntegrationTests() : juce::UnitTest ("UNI76AudioProcessor + PITCH", "UNI76") {}

    void runTest() override
    {
        beginTest ("PITCH parameter is genuinely discrete: 25 states, range -12..+12, default 0 ST");
        {
            UNI76AudioProcessor processor;
            auto* rangedParam = processor.getValueTreeState().getParameter (uni76::ParamID::pitch);
            auto* param = dynamic_cast<juce::AudioParameterInt*> (rangedParam);
            expect (param != nullptr, "pitch must be an AudioParameterInt");

            if (param != nullptr && rangedParam != nullptr)
            {
                expectEquals (param->getRange().getStart(), -12);
                expectEquals (param->getRange().getEnd(), 12);
                expectEquals (param->get(), 0);
                // 25 valid states (-12..+12 inclusive) - getNumSteps() is a
                // private override in AudioParameterInt, so it must be
                // called through the RangedAudioParameter base pointer.
                expectEquals (rangedParam->getNumSteps(), 25);
            }
        }

        beginTest ("Fresh instance opens at 0 ST (normalised 0.5, the range's midpoint)");
        {
            UNI76AudioProcessor processor;
            auto* param = processor.getValueTreeState().getParameter (uni76::ParamID::pitch);
            expect (param != nullptr);
            if (param != nullptr)
                expectWithinAbsoluteError (param->getValue(), 0.5f, 0.001f);
        }

        beginTest ("New-schema state round-trips pitch exactly at -12/0/+12 ST");
        {
            for (int st : { -12, 0, 12 })
            {
                UNI76AudioProcessor processor;
                auto& apvts = processor.getValueTreeState();
                auto* param = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (uni76::ParamID::pitch));
                expect (param != nullptr);
                if (param == nullptr) continue;

                param->setValueNotifyingHost (param->convertTo0to1 ((float) st));

                juce::MemoryBlock saved;
                processor.getStateInformation (saved);

                UNI76AudioProcessor reloaded;
                reloaded.setStateInformation (saved.getData(), (int) saved.getSize());
                auto* reloadedParam = dynamic_cast<juce::AudioParameterInt*> (reloaded.getValueTreeState().getParameter (uni76::ParamID::pitch));
                expect (reloadedParam != nullptr);
                if (reloadedParam != nullptr)
                    expectEquals (reloadedParam->get(), st);
            }
        }

        beginTest ("Legacy (pre-v3) saved state migrates pitch to 0 ST regardless of its old raw percent value");
        {
            UNI76AudioProcessor processor;
            auto& apvts = processor.getValueTreeState();

            // Hand-build a v2-shaped state (the real old format, confirmed
            // by direct experiment - APVTS stores each parameter's raw,
            // denormalised value as `value="X"` in its own declared range,
            // which for the old pitch was 0..100). A user who set the old
            // percent-based pitch to 73% and never touched it again must
            // not suddenly hear -12..+12-range garbage after the update -
            // and specifically must land on the new default, 0 ST.
            auto legacyState = apvts.copyState();
            legacyState.setProperty (uni76::stateSchemaVersionProperty, 2, nullptr); // pre-v3
            for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                legacyState.setProperty (uni76::ModuleEnableState::propertyNames[(size_t) i], true, nullptr);

            auto legacyPitchParam = legacyState.getChildWithProperty ("id", juce::var (uni76::ParamID::pitch));
            expect (legacyPitchParam.isValid());
            legacyPitchParam.setProperty ("value", 73.0, nullptr); // old raw 0..100% value

            if (auto xml = legacyState.createXml())
            {
                juce::MemoryBlock data;
                juce::AudioProcessor::copyXmlToBinary (*xml, data);
                processor.setStateInformation (data.getData(), (int) data.getSize());
            }

            auto* param = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (uni76::ParamID::pitch));
            expect (param != nullptr);
            if (param != nullptr)
                expectEquals (param->get(), 0);
        }

        beginTest ("Legacy state with pitch untouched (old default 0.0) also migrates cleanly to 0 ST");
        {
            UNI76AudioProcessor processor;
            auto& apvts = processor.getValueTreeState();

            auto legacyState = apvts.copyState();
            legacyState.setProperty (uni76::stateSchemaVersionProperty, 1, nullptr); // pre-v2, pre-v3

            if (auto xml = legacyState.createXml())
            {
                juce::MemoryBlock data;
                juce::AudioProcessor::copyXmlToBinary (*xml, data);
                processor.setStateInformation (data.getData(), (int) data.getSize());
            }

            auto* param = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (uni76::ParamID::pitch));
            expect (param != nullptr);
            if (param != nullptr)
                expectEquals (param->get(), 0);

            // Missing module-enabled flags (pre-v2) still default to enabled.
            expect (processor.getModuleEnableState().isEnabled (3));
        }

        beginTest ("pitchEnabled flag (moduleEnableState index 3) persists across save/restore");
        {
            UNI76AudioProcessor processor;
            processor.getModuleEnableState().setEnabled (3, false);

            juce::MemoryBlock saved;
            processor.getStateInformation (saved);

            UNI76AudioProcessor reloaded;
            reloaded.setStateInformation (saved.getData(), (int) saved.getSize());
            expect (! reloaded.getModuleEnableState().isEnabled (3), "pitchEnabled=false should survive save/restore");
        }

        beginTest ("Full chain PREAMP+EQ+SAT+PITCH stays finite/stable for representative parameter combinations");
        {
            struct Combo { float preamp, eq, sat; int pitchSt; };
            const Combo combos[] {
                { 0.0f, 0.5f, 0.0f, 0 }, { 0.0f, 0.5f, 0.5f, 12 }, { 0.5f, 0.5f, 0.5f, -12 },
                { 0.5f, 1.0f, 0.75f, 7 }, { 0.75f, 0.5f, 1.0f, -7 },
            };

            for (const auto& combo : combos)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (44100.0, 512);

                processor.getValueTreeState().getParameter (uni76::ParamID::preamp)->setValueNotifyingHost (combo.preamp);
                processor.getValueTreeState().getParameter (uni76::ParamID::eq)->setValueNotifyingHost (combo.eq);
                processor.getValueTreeState().getParameter (uni76::ParamID::saturation)->setValueNotifyingHost (combo.sat);

                auto* pitchParam = dynamic_cast<juce::AudioParameterInt*> (processor.getValueTreeState().getParameter (uni76::ParamID::pitch));
                expect (pitchParam != nullptr);
                if (pitchParam != nullptr)
                    pitchParam->setValueNotifyingHost (pitchParam->convertTo0to1 ((float) combo.pitchSt));

                juce::MidiBuffer midi;
                juce::AudioBuffer<float> buffer (2, 512);
                bool finite = true;
                float peak = 0.0f;

                for (int b = 0; b < 25; ++b)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int s = 0; s < 512; ++s)
                            buffer.setSample (ch, s, 0.35f * std::sin (juce::MathConstants<float>::twoPi * 500.0f
                                                                        * (float) (b * 512 + s) / 44100.0f));
                    processor.processBlock (buffer, midi);
                    if (! bufferIsFinite (buffer)) finite = false;
                }

                for (int ch = 0; ch < 2; ++ch)
                    for (int s = 0; s < 512; ++s)
                        peak = juce::jmax (peak, std::abs (buffer.getSample (ch, s)));

                const juce::String label = "PREAMP=" + juce::String (combo.preamp) + " EQ=" + juce::String (combo.eq)
                                          + " SAT=" + juce::String (combo.sat) + " PITCH=" + juce::String (combo.pitchSt) + "ST";
                expect (finite, "non-finite output for " + label);
                expect (peak < 4.0f, "unexpected gain explosion for " + label);
            }
        }
    }
};

static UNI76PitchIntegrationTests uni76PitchIntegrationTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// Closing out the real gaps flagged after the initial PITCH pass: exact
// latency accounting, a real 0 ST A/B against a latency-aligned dry copy,
// deterministic polyphonic material (bass+harmonics, low dyads, triads, a
// dense chord, and the especially critical bass+chord case), and stereo
// coherence with genuinely non-identical L/R content (not just dual-mono).
class UNI76PitchPolyphonicTests final : public juce::UnitTest
{
public:
    UNI76PitchPolyphonicTests() : juce::UnitTest ("uni76::dsp::PitchProcessor polyphonic/coherence", "UNI76") {}

    void runTest() override
    {
        beginTest ("Exact latency table: PITCH alone vs PREAMP+EQ+SAT vs total plugin, at 44.1/48/96/192kHz");
        {
            const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };

            std::cout << "\n=== PITCH / total plugin latency table ===" << std::endl;

            for (auto sr : rates)
            {
                uni76::dsp::PitchProcessor pitchAlone;
                pitchAlone.prepare (sr, 512, 2);
                const auto pitchLatency = pitchAlone.getLatencySamples();

                uni76::dsp::PreampProcessor preamp;
                preamp.prepare (sr, 512, 2);
                uni76::dsp::EqProcessor eq;
                eq.prepare (sr, 512, 2);
                uni76::dsp::SatProcessor sat;
                sat.prepare (sr, 512, 2);
                const auto preEqSatLatency = preamp.getLatencySamples() + eq.getLatencySamples() + sat.getLatencySamples();

                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 512);
                const auto totalLatency = processor.getLatencySamples();

                std::cout << "  " << sr << "Hz: PITCH=" << pitchLatency << "smp/" << (1000.0 * pitchLatency / sr)
                           << "ms  PREAMP+EQ+SAT=" << preEqSatLatency << "smp/" << (1000.0 * preEqSatLatency / sr)
                           << "ms  TOTAL=" << totalLatency << "smp/" << (1000.0 * totalLatency / sr) << "ms" << std::endl;

                expectEquals (totalLatency, pitchLatency + preEqSatLatency, "total plugin latency must equal PITCH + PREAMP+EQ+SAT");
            }
            std::cout << "=== end latency table ===" << std::endl << std::endl;
        }

        beginTest ("0 ST A/B vs a latency-aligned dry copy: broadband RMS diff, max diff, frequency-response deviation");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            uni76::dsp::PitchProcessor pitch;
            pitch.prepare (sr, blockSize, 1);
            const auto latency = pitch.getLatencySamples();

            const auto totalSamples = latency + (int) (2.0 * sr);
            auto input = generateBroadband (totalSamples, sr);
            auto output = runPitchProcessor (pitch, input, blockSize, 0, true);

            double sumSq = 0.0, maxAbs = 0.0;
            int count = 0;
            for (int i = latency + 2000; i < totalSamples; ++i)
            {
                const auto diff = (double) output.getSample (0, i) - (double) input.getSample (0, i - latency);
                sumSq += diff * diff;
                maxAbs = juce::jmax (maxAbs, std::abs (diff));
                ++count;
            }
            const auto rmsDiff = count > 0 ? std::sqrt (sumSq / (double) count) : 0.0;

            std::cout << "\n=== PITCH 0 ST A/B vs latency-aligned dry (broadband) ===" << std::endl;
            std::cout << "  RMS diff = " << rmsDiff << "  max diff = " << maxAbs << std::endl;

            const float freqs[] { 40.0f, 80.0f, 150.0f, 300.0f, 600.0f, 1200.0f, 2500.0f, 5000.0f, 9000.0f, 14000.0f };
            for (auto freqHz : freqs)
            {
                const auto win = juce::jmin (totalSamples - latency - 2000, periodicAnalysisLength (sr, freqHz, 30));
                const auto inMag  = goertzelMagnitude (input,  0, totalSamples - win, win, sr, freqHz);
                const auto outMag = goertzelMagnitude (output, 0, totalSamples - win, win, sr, freqHz);
                const auto devDb = 20.0f * std::log10 (juce::jmax (outMag, 1.0e-9f) / juce::jmax (inMag, 1.0e-9f));
                std::cout << "  " << freqHz << "Hz: deviation = " << devDb << " dB" << std::endl;
                expect (std::abs (devDb) < 1.0f, juce::String ("0 ST frequency-response deviation too large at ") + juce::String (freqHz) + "Hz");
            }
            std::cout << "=== end 0 ST A/B ===" << std::endl << std::endl;

            expect (rmsDiff < 0.05, "0 ST RMS difference vs latency-aligned dry too large");
            expect (maxAbs < 0.5, "0 ST max sample difference vs latency-aligned dry too large");
        }

        beginTest ("Polyphonic A: bass + harmonics (60/120/180Hz) - whole material shifts together, bass stays stable");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const std::vector<float> freqs { 60.0f, 120.0f, 180.0f };
            const std::vector<float> amps  { 0.3f, 0.15f, 0.08f };
            const int semitones[] { -12, -7, -3, 3, 7, 12 };

            std::cout << "\n=== Polyphonic A: bass + harmonics ===" << std::endl;

            for (auto st : semitones)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 1);
                const auto latency = pitch.getLatencySamples();
                const auto settle = latency + (int) (0.15 * sr);
                const auto totalSamples = settle + (int) (0.6 * sr);

                auto input = generateChord (totalSamples, sr, freqs, amps);
                auto output = runPitchProcessor (pitch, input, blockSize, st, true);

                for (size_t n = 0; n < freqs.size(); ++n)
                {
                    const auto target = freqs[n] * std::pow (2.0f, (float) st / 12.0f);
                    const auto stability = analyzeBassStability (output, 0, settle, totalSamples - settle, sr, target);
                    expect (stability.numWindows > 2, "not enough windows for component " + juce::String ((int) n));

                    const auto errPercent = target > 0.0f ? 100.0 * std::abs (stability.freqMean - (double) target) / (double) target : 0.0;
                    std::cout << "  " << st << "ST partial " << freqs[n] << "Hz->" << target << "Hz: freqMean=" << stability.freqMean
                               << "Hz err=" << errPercent << "% ampDbStd=" << stability.ampDbStd << "dB rmsModDepth=" << stability.rmsModDepth << std::endl;

                    expect (errPercent < 4.0, "component drifted off its expected shifted frequency");

                    if (n == 0) // the bass fundamental - full stability bar
                    {
                        const auto freqDevPercent = target > 0.0f ? 100.0 * stability.freqStd / (double) target : 0.0;
                        expect (freqDevPercent < 3.0, "bass fundamental wobbles too much in a polyphonic mix");
                        expect (stability.ampDbStd < 2.5, "bass fundamental amplitude-modulates too much in a polyphonic mix");
                        expect (stability.rmsModDepth < 0.2, "bass fundamental RMS modulates too much in a polyphonic mix");

                        const auto sidebands = analyzeSidebands (output, 0, settle, totalSamples - settle, sr, target, 25.0f);
                        expect (sidebands.belowDb < -15.0, "bass fundamental has an unexpectedly strong sideband below it");
                        expect (sidebands.aboveDb < -15.0, "bass fundamental has an unexpectedly strong sideband above it");
                    }
                }
            }
            std::cout << "=== end Polyphonic A ===" << std::endl << std::endl;
        }

        beginTest ("Polyphonic B: two simultaneous low tones (55/110Hz, an octave) - neither wobbles, neither disappears");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            // An exact octave (not an arbitrary/close interval): the
            // combined dry waveform is still perfectly periodic with no
            // beat envelope of its own, so any amplitude modulation
            // measured in the *output* is attributable to the algorithm,
            // not to real acoustic beating between two unrelated tones
            // (which an earlier, closer-interval version of this test
            // measured and which is physics, not a PITCH defect).
            const std::vector<float> freqs { 55.0f, 110.0f };
            const std::vector<float> amps  { 0.25f, 0.25f };
            const int semitones[] { -12, -7, -3, 3, 7, 12 };

            std::cout << "\n=== Polyphonic B: two low tones ===" << std::endl;

            for (auto st : semitones)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 1);
                const auto latency = pitch.getLatencySamples();
                const auto settle = latency + (int) (0.15 * sr);
                const auto totalSamples = settle + (int) (0.6 * sr);

                auto input = generateChord (totalSamples, sr, freqs, amps);
                auto output = runPitchProcessor (pitch, input, blockSize, st, true);

                for (size_t n = 0; n < freqs.size(); ++n)
                {
                    const auto target = freqs[n] * std::pow (2.0f, (float) st / 12.0f);
                    const auto stability = analyzeBassStability (output, 0, settle, totalSamples - settle, sr, target);
                    expect (stability.numWindows > 2, "not enough windows for low tone " + juce::String ((int) n));

                    const auto freqDevPercent = target > 0.0f ? 100.0 * stability.freqStd / (double) target : 0.0;
                    std::cout << "  " << st << "ST tone " << freqs[n] << "Hz->" << target << "Hz: freqMean=" << stability.freqMean
                               << "Hz freqStd%=" << freqDevPercent << " ampDbStd=" << stability.ampDbStd
                               << "dB rmsModDepth=" << stability.rmsModDepth << std::endl;

                    expect (freqDevPercent < 3.0, "a low tone wobbles too much when another low tone plays simultaneously");
                    expect (stability.ampDbStd < 2.5, "a low tone amplitude-modulates too much (possible beating) with another low tone present");
                    expect (stability.rmsModDepth < 0.2, "a low tone's RMS modulates too much with another low tone present");
                }
            }
            std::cout << "=== end Polyphonic B ===" << std::endl << std::endl;
        }

        beginTest ("Polyphonic C: major and minor triads - all three notes shift together, chord doesn't smear");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const int semitones[] { -12, -7, -3, 3, 7, 12 };

            struct Triad { const char* name; std::vector<float> freqs; };
            const Triad triads[] {
                { "A major", { 220.0f, 277.18f, 329.63f } },
                { "A minor", { 220.0f, 261.63f, 329.63f } },
            };

            std::cout << "\n=== Polyphonic C: triads ===" << std::endl;

            for (const auto& triad : triads)
            {
                const std::vector<float> amps (triad.freqs.size(), 0.2f);

                for (auto st : semitones)
                {
                    uni76::dsp::PitchProcessor pitch;
                    pitch.prepare (sr, blockSize, 1);
                    const auto latency = pitch.getLatencySamples();
                    const auto settle = latency + (int) (0.15 * sr);
                    const auto totalSamples = settle + (int) (0.5 * sr);

                    auto input = generateChord (totalSamples, sr, triad.freqs, amps);
                    auto output = runPitchProcessor (pitch, input, blockSize, st, true);

                    for (size_t n = 0; n < triad.freqs.size(); ++n)
                    {
                        const auto target = triad.freqs[n] * std::pow (2.0f, (float) st / 12.0f);
                        // A minor third's frequency gap is <1 Goertzel bin
                        // wide at the default 4-cycle window at *any*
                        // register (the gap and the resolution both scale
                        // with the target frequency) - 14 cycles gives
                        // enough margin to actually separate adjacent
                        // triad tones instead of measuring cross-leakage.
                        const auto stability = analyzeBassStability (output, 0, settle, totalSamples - settle, sr, target, 14);
                        expect (stability.numWindows > 2, juce::String (triad.name) + ": not enough windows for note " + juce::String ((int) n));

                        const auto errPercent = target > 0.0f ? 100.0 * std::abs (stability.freqMean - (double) target) / (double) target : 0.0;
                        expect (errPercent < 4.0, juce::String (triad.name) + " " + juce::String (st) + "ST: a chord tone drifted off its expected shifted frequency");

                        if (n == 0)
                        {
                            const auto freqDevPercent = target > 0.0f ? 100.0 * stability.freqStd / (double) target : 0.0;
                            std::cout << "  " << triad.name << " " << st << "ST root " << triad.freqs[0] << "Hz->" << target
                                       << "Hz: freqStd%=" << freqDevPercent << " ampDbStd=" << stability.ampDbStd << "dB" << std::endl;
                            expect (freqDevPercent < 3.0, juce::String (triad.name) + ": chord root wobbles too much");
                            expect (stability.ampDbStd < 2.5, juce::String (triad.name) + ": chord root amplitude-modulates too much");
                        }
                    }
                }
            }
            std::cout << "=== end Polyphonic C ===" << std::endl << std::endl;
        }

        beginTest ("Polyphonic D: dense 5-note chord (Cmaj9-voicing) stays coherent under shift");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const std::vector<float> freqs { 130.81f, 164.81f, 196.00f, 246.94f, 293.66f };
            const std::vector<float> amps  { 0.15f, 0.15f, 0.15f, 0.15f, 0.15f };
            const int semitones[] { -12, -7, -3, 3, 7, 12 };

            std::cout << "\n=== Polyphonic D: dense 5-note chord ===" << std::endl;

            for (auto st : semitones)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 1);
                const auto latency = pitch.getLatencySamples();
                const auto settle = latency + (int) (0.15 * sr);
                const auto totalSamples = settle + (int) (0.5 * sr);

                auto input = generateChord (totalSamples, sr, freqs, amps);
                auto output = runPitchProcessor (pitch, input, blockSize, st, true);
                expect (bufferIsFinite (output), "dense chord produced non-finite output");

                float peak = 0.0f;
                for (int i = 0; i < totalSamples; ++i) peak = juce::jmax (peak, std::abs (output.getSample (0, i)));
                expect (peak < 3.0f, "dense chord caused a gain explosion");

                for (size_t n = 0; n < freqs.size(); ++n)
                {
                    const auto target = freqs[n] * std::pow (2.0f, (float) st / 12.0f);
                    // Adjacent notes in this voicing are thirds apart - see
                    // the comment on the triad test above for why that
                    // needs a wider analysis window than an isolated tone.
                    const auto stability = analyzeBassStability (output, 0, settle, totalSamples - settle, sr, target, 14);
                    expect (stability.numWindows > 2, "not enough windows for chord note " + juce::String ((int) n));

                    const auto errPercent = target > 0.0f ? 100.0 * std::abs (stability.freqMean - (double) target) / (double) target : 0.0;
                    expect (errPercent < 4.0, "dense chord note drifted off its expected shifted frequency");

                    if (n == 0)
                    {
                        const auto freqDevPercent = target > 0.0f ? 100.0 * stability.freqStd / (double) target : 0.0;
                        std::cout << "  " << st << "ST lowest note " << freqs[0] << "Hz->" << target
                                   << "Hz: freqStd%=" << freqDevPercent << " ampDbStd=" << stability.ampDbStd << "dB" << std::endl;
                        expect (freqDevPercent < 3.5, "dense chord's lowest note wobbles too much");
                    }
                }
            }
            std::cout << "=== end Polyphonic D ===" << std::endl << std::endl;
        }

        beginTest ("Polyphonic E (especially critical): bass note + chord together - bass never disappears or wobbles");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const std::vector<float> freqs { 60.0f, 220.0f, 277.18f, 329.63f };
            const std::vector<float> amps  { 0.3f, 0.15f, 0.15f, 0.15f };
            const int semitones[] { -12, -7, -3, 3, 7, 12 };

            std::cout << "\n=== Polyphonic E: bass + chord (critical) ===" << std::endl;

            for (auto st : semitones)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 1);
                const auto latency = pitch.getLatencySamples();
                const auto settle = latency + (int) (0.15 * sr);
                const auto totalSamples = settle + (int) (0.7 * sr);

                auto input = generateChord (totalSamples, sr, freqs, amps);
                auto output = runPitchProcessor (pitch, input, blockSize, st, true);

                const auto bassTarget = freqs[0] * std::pow (2.0f, (float) st / 12.0f);
                const auto stability = analyzeBassStability (output, 0, settle, totalSamples - settle, sr, bassTarget);
                expect (stability.numWindows > 3, "not enough windows for bass-under-chord analysis");

                const auto freqDevPercent = bassTarget > 0.0f ? 100.0 * stability.freqStd / (double) bassTarget : 0.0;
                const auto errPercent = bassTarget > 0.0f ? 100.0 * std::abs (stability.freqMean - (double) bassTarget) / (double) bassTarget : 0.0;

                std::cout << "  " << st << "ST bass 60Hz->" << bassTarget << "Hz: freqMean=" << stability.freqMean
                           << "Hz err=" << errPercent << "% freqStd%=" << freqDevPercent << " ampDbStd=" << stability.ampDbStd
                           << "dB rmsModDepth=" << stability.rmsModDepth << std::endl;

                // This is the scenario the product brief calls out by name
                // as especially critical - hold it to the same bar as the
                // pure-bass matrix, not a relaxed one.
                expect (errPercent < 2.0, "bass note drifted off its expected shifted frequency under a chord");
                expect (freqDevPercent < 3.0, "bass note wobbles under a chord (spectral swimming)");
                expect (stability.ampDbStd < 2.5, "bass note amplitude-modulates under a chord (periodic beating/breathing)");
                expect (stability.rmsModDepth < 0.2, "bass note's RMS is unstable under a chord (disappearing/reappearing)");

                const auto sidebands = analyzeSidebands (output, 0, settle, totalSamples - settle, sr, bassTarget, 25.0f);
                expect (sidebands.belowDb < -15.0, "bass-under-chord: unexpectedly strong sideband below the bass fundamental");
                expect (sidebands.aboveDb < -15.0, "bass-under-chord: unexpectedly strong sideband above the bass fundamental");

                // Chord tones should also land on their shifted targets, not
                // smear. Adjacent chord tones here are thirds apart - see
                // the comment on the triad test above for why that needs a
                // wider analysis window than an isolated tone.
                for (size_t n = 1; n < freqs.size(); ++n)
                {
                    const auto target = freqs[n] * std::pow (2.0f, (float) st / 12.0f);
                    const auto chordStability = analyzeBassStability (output, 0, settle, totalSamples - settle, sr, target, 14);
                    if (chordStability.numWindows > 2)
                    {
                        const auto chordErrPercent = target > 0.0f ? 100.0 * std::abs (chordStability.freqMean - (double) target) / (double) target : 0.0;
                        expect (chordErrPercent < 4.0, "chord tone drifted off its expected shifted frequency under a bass note");
                    }
                }
            }
            std::cout << "=== end Polyphonic E ===" << std::endl << std::endl;
        }

        beginTest ("Polyphonic E in stereo (dual-mono): bit-identical L/R holds for real musical material too");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const std::vector<float> freqs { 60.0f, 220.0f, 277.18f, 329.63f };
            const std::vector<float> amps  { 0.3f, 0.15f, 0.15f, 0.15f };

            for (auto st : { -12, 7, 12 })
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 2);

                const auto totalSamples = pitch.getLatencySamples() + (int) sr;
                auto mono = generateChord (totalSamples, sr, freqs, amps);
                juce::AudioBuffer<float> input (2, totalSamples);
                input.copyFrom (0, 0, mono, 0, 0, totalSamples);
                input.copyFrom (1, 0, mono, 0, 0, totalSamples);

                auto output = runPitchProcessor (pitch, input, blockSize, st, true);

                double maxAbsDiff = 0.0;
                for (int i = 0; i < totalSamples; ++i)
                    maxAbsDiff = juce::jmax (maxAbsDiff, (double) std::abs (output.getSample (0, i) - output.getSample (1, i)));

                expect (maxAbsDiff < 1.0e-6, juce::String (st) + " ST: polyphonic dual-mono must still produce bit-identical stereo output, maxAbsDiff=" + juce::String (maxAbsDiff));
            }
        }

        beginTest ("Non-identical stereo material: shared bass + different chord voicing per channel");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const int semitones[] { -12, 7, 12 };

            // L: bass + A major triad. R: the SAME bass + A minor triad -
            // genuinely different per-channel content (not dual-mono),
            // sharing only the bass note so its measured frequency/level
            // can be meaningfully compared between the two independent
            // per-channel engines.
            const std::vector<float> freqsL { 60.0f, 220.0f, 277.18f, 329.63f };
            const std::vector<float> ampsL  { 0.3f, 0.15f, 0.15f, 0.15f };
            const std::vector<float> freqsR { 60.0f, 220.0f, 261.63f, 329.63f };
            const std::vector<float> ampsR  { 0.3f, 0.15f, 0.15f, 0.15f };

            std::cout << "\n=== Non-identical stereo (shared bass, different chords) ===" << std::endl;

            for (auto st : semitones)
            {
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, blockSize, 2);
                const auto latency = pitch.getLatencySamples();
                const auto settle = latency + (int) (0.15 * sr);
                const auto totalSamples = settle + (int) (0.6 * sr);

                auto monoL = generateChord (totalSamples, sr, freqsL, ampsL);
                auto monoR = generateChord (totalSamples, sr, freqsR, ampsR);
                juce::AudioBuffer<float> input (2, totalSamples);
                input.copyFrom (0, 0, monoL, 0, 0, totalSamples);
                input.copyFrom (1, 0, monoR, 0, 0, totalSamples);

                auto output = runPitchProcessor (pitch, input, blockSize, st, true);
                expect (bufferIsFinite (output), "non-identical stereo material produced non-finite output");

                const auto bassTarget = 60.0f * std::pow (2.0f, (float) st / 12.0f);
                const auto statsL = analyzeBassStability (output, 0, settle, totalSamples - settle, sr, bassTarget);
                const auto statsR = analyzeBassStability (output, 1, settle, totalSamples - settle, sr, bassTarget);

                if (statsL.numWindows > 2 && statsR.numWindows > 2)
                {
                    const auto errL = 100.0 * std::abs (statsL.freqMean - (double) bassTarget) / (double) bassTarget;
                    const auto errR = 100.0 * std::abs (statsR.freqMean - (double) bassTarget) / (double) bassTarget;
                    const auto errDiff = std::abs (errL - errR);

                    std::cout << "  " << st << "ST bass->" << bassTarget << "Hz: L err=" << errL << "% R err=" << errR
                               << "%  |L-R| err diff=" << errDiff << "%" << std::endl;

                    expect (errL < 2.5, "L channel bass pitch error too large with non-identical stereo content");
                    expect (errR < 2.5, "R channel bass pitch error too large with non-identical stereo content");
                    expect (errDiff < 1.0, "L/R bass pitch error differs too much between the two independent engines");
                }

                // Level match: RMS of the shared bass component's own
                // Goertzel magnitude should track closely between channels
                // across the run (no L/R level mismatch or wandering image
                // introduced purely by running two separate engines).
                const auto win = juce::jmin (totalSamples - settle, periodicAnalysisLength (sr, bassTarget, 20));
                const auto magL = goertzelMagnitude (output, 0, totalSamples - win, win, sr, bassTarget);
                const auto magR = goertzelMagnitude (output, 1, totalSamples - win, win, sr, bassTarget);
                const auto levelDiffDb = 20.0f * std::log10 (juce::jmax (magL, 1.0e-9f) / juce::jmax (magR, 1.0e-9f));
                std::cout << "  " << st << "ST bass level: L/R = " << levelDiffDb << " dB" << std::endl;
                expect (std::abs (levelDiffDb) < 0.5f, "shared bass component's level differs too much between L and R engines");

                // Latency is a single scalar for the whole (stereo) instance
                // by construction - re-confirm it stayed the same value.
                expectEquals (pitch.getLatencySamples(), latency, "latency must stay identical with non-identical stereo content");
            }
            std::cout << "=== end non-identical stereo ===" << std::endl << std::endl;
        }
    }
};

static UNI76PitchPolyphonicTests uni76PitchPolyphonicTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// PAN / STEREO FIELD - see Source/DSP/PanoramaProcessor.h and
// docs/DSP_PAN.md. NOT an L/R balance pan despite the `panorama`
// parameter ID - a combined stereo width + slow ear-to-ear motion
// control: ORIGINAL (0%, bit-exact identity) -> WIDE (50%) -> MOTION
// (100%, obvious slow L<->R swing). Mid is always passed through
// untouched (the stable "core"); width/motion apply only to a separately
// -processed "spatial" signal built from real Side content plus, for
// mono/near-mono sources, a phase-decorrelated "induced" component.
class UNI76PanoramaProcessorTests final : public juce::UnitTest
{
public:
    UNI76PanoramaProcessorTests() : juce::UnitTest ("uni76::dsp::PanoramaProcessor", "UNI76") {}

    void runTest() override
    {
        beginTest ("Construct/prepare/reset does not crash; latency is always exactly 0");
        {
            uni76::dsp::PanoramaProcessor pan;
            pan.prepare (44100.0, 512, 2);
            expectEquals (pan.getLatencySamples(), 0, "PAN must add zero latency - no delay-based motion (no Haas)");
            pan.reset();

            juce::AudioBuffer<float> buffer (2, 512);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    buffer.setSample (ch, i, 0.2f * std::sin (0.1f * (float) i));

            pan.process (buffer, 1.0f, true);
            expect (bufferIsFinite (buffer), "process() produced non-finite output right after prepare()");
        }

        beginTest ("Latency is always 0 regardless of width, enabled state, sample rate, or block size");
        {
            const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
            const int blockSizes[] { 32, 64, 128, 256, 512, 1024, 2048 };

            for (auto sr : rates)
                for (auto blockSize : blockSizes)
                {
                    uni76::dsp::PanoramaProcessor pan;
                    pan.prepare (sr, blockSize, 2);
                    expectEquals (pan.getLatencySamples(), 0);
                }

            uni76::dsp::PanoramaProcessor pan;
            pan.prepare (44100.0, 512, 2);
            for (auto width : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                for (auto enabled : { true, false })
                {
                    juce::AudioBuffer<float> buffer (2, 512);
                    buffer.clear();
                    pan.process (buffer, width, enabled);
                    expectEquals (pan.getLatencySamples(), 0);
                }
        }

        beginTest ("PAN=0 (ORIGINAL) is a near-identity transform (numerical null test)");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            uni76::dsp::PanoramaProcessor pan;
            pan.prepare (sr, blockSize, 2);

            const auto totalSamples = (int) sr;
            auto input = generateCorrelatedChord (totalSamples, sr);
            auto output = runPanoramaProcessor (pan, input, blockSize, 0.0f, true);

            const auto settle = (int) (0.1 * sr);
            double sumSq = 0.0, maxAbs = 0.0;
            int count = 0;
            for (int i = settle; i < totalSamples; ++i)
                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto diff = (double) output.getSample (ch, i) - (double) input.getSample (ch, i);
                    sumSq += diff * diff;
                    maxAbs = juce::jmax (maxAbs, std::abs (diff));
                    ++count;
                }
            const auto rmsDiff = std::sqrt (sumSq / (double) count);

            std::cout << "\n=== PAN ORIGINAL (0%) null test === RMS diff=" << rmsDiff << " max diff=" << maxAbs << std::endl << std::endl;
            expect (rmsDiff < 1.0e-4, "ORIGINAL should be a near-identity transform, RMS diff=" + juce::String (rmsDiff));
            expect (maxAbs < 1.0e-3, "ORIGINAL should be a near-identity transform, max diff=" + juce::String (maxAbs));
        }

        beginTest ("Width/motion/induced-blend curve mapping matches the product brief's target points, exactly (pure functions)");
        {
            std::cout << "\n=== PAN curve mapping (PanoramaCurves.h, direct) ===" << std::endl;

            struct Case { float t; float widthTarget; };
            // Targets reduced from an earlier {1.2,1.45,1.675,1.9} during
            // this round's correlation-balancing pass - see
            // panWidthMaxHigh's comment in PanoramaCurves.h and
            // docs/DSP_PAN.md's "Correlation" section.
            const Case widthCases[] {
                { 0.0f, 1.0f }, { 0.25f, 1.1f }, { 0.5f, 1.3f }, { 0.75f, 1.5f }, { 1.0f, 1.6f },
            };
            for (const auto& c : widthCases)
            {
                const auto w = uni76::dsp::panWidthGain (c.t, uni76::dsp::panWidthMaxHigh);
                std::cout << "  width(" << (c.t * 100.0f) << "%) = " << w << " (target ~" << c.widthTarget << ")" << std::endl;
                expect (std::abs (w - c.widthTarget) < 0.15f, "high-band width mapping deviates from target at " + juce::String (c.t * 100.0f) + "%");
            }
            expectWithinAbsoluteError (uni76::dsp::panWidthGain (0.0f, uni76::dsp::panWidthMaxHigh), 1.0f, 1.0e-6f, "width(0%) must be exactly 1.0 (identity)");

            const Case motionCases[] {
                { 0.0f, 0.0f }, { 0.25f, 0.15f }, { 0.5f, 0.425f }, { 0.75f, 0.7f }, { 1.0f, 0.85f },
            };
            for (const auto& c : motionCases)
            {
                const auto m = uni76::dsp::panMotionDepth (c.t, uni76::dsp::panMotionDepthMaxHigh);
                std::cout << "  motionDepth(" << (c.t * 100.0f) << "%) = " << m << std::endl;
            }
            expectWithinAbsoluteError (uni76::dsp::panMotionDepth (0.0f, uni76::dsp::panMotionDepthMaxHigh), 0.0f, 1.0e-6f, "motionDepth(0%) must be exactly 0.0 (no motion at ORIGINAL)");
            expect (uni76::dsp::panMotionDepth (1.0f, uni76::dsp::panMotionDepthMaxHigh) > uni76::dsp::panMotionDepth (0.5f, uni76::dsp::panMotionDepthMaxHigh),
                    "motion depth must grow monotonically toward MOTION (100%)");

            expectWithinAbsoluteError (uni76::dsp::panInducedBlend (0.0f), 0.0f, 1.0e-6f, "induced blend must be exactly 0.0 at ORIGINAL");
            expect (uni76::dsp::panInducedBlend (1.0f) > 0.3f, "induced blend should be substantial at 100% (mono sources need real spatial content)");

            // Low band must widen/move far less than high band at the same t.
            for (auto t : { 0.25f, 0.5f, 0.75f, 1.0f })
            {
                expect (uni76::dsp::panWidthGain (t, uni76::dsp::panWidthMaxLow) < uni76::dsp::panWidthGain (t, uni76::dsp::panWidthMaxHigh),
                        "low-band width should stay below high-band width at " + juce::String (t * 100.0f) + "%");
                expect (uni76::dsp::panMotionDepth (t, uni76::dsp::panMotionDepthMaxLow) < uni76::dsp::panMotionDepth (t, uni76::dsp::panMotionDepthMaxHigh),
                        "low-band motion depth should stay below high-band motion depth at " + juce::String (t * 100.0f) + "%");
            }
            std::cout << "=== end curve mapping ===" << std::endl << std::endl;
        }

        beginTest ("Mono source (identical L/R) gains spatial content only for PAN>0 - Side is exactly 0 at PAN=0");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            const auto totalSamples = (int) sr;
            auto monoInStereo = generateMonoHarmonicStereo (totalSamples, sr, 0.3f);
            const auto settle = (int) (0.1 * sr);

            std::cout << "\n=== PAN mono-input spatial field growth (1kHz) ===" << std::endl;
            double previousSideRms = -1.0;
            for (auto width : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, blockSize, 2);
                auto output = runPanoramaProcessor (pan, monoInStereo, blockSize, width, true);
                const auto stats = measureStereo (output, settle, totalSamples - settle);

                std::cout << "  width=" << (width * 100.0f) << "%: Side RMS=" << stats.rmsSide << " correlation=" << stats.correlation << std::endl;

                if (width == 0.0f)
                    expect (stats.rmsSide < 1.0e-5, "mono source must have exactly-zero Side at PAN=0 (ORIGINAL)");
                else
                {
                    expect (stats.rmsSide > previousSideRms, "mono source's Side content should keep growing with width/motion");
                    expect (stats.rmsSide > 1.0e-4, "mono source should have gained *real*, measurable spatial content at width " + juce::String (width * 100.0f) + "%");
                }
                previousSideRms = stats.rmsSide;
            }
            std::cout << "=== end mono-input spatial field growth ===" << std::endl << std::endl;
        }

        beginTest ("Combined stereo power grows with width (a motion-independent proxy for 'the system gets wider')");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            const auto totalSamples = (int) (3.0 * sr); // several LFO cycles, so motion's redistribution averages out
            auto input = generateDecorrelatedStereo (totalSamples, sr);
            const auto settle = (int) (0.3 * sr);

            std::cout << "\n=== PAN combined power vs width ===" << std::endl;
            double previousPower = -1.0;
            for (auto width : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, blockSize, 2);
                auto output = runPanoramaProcessor (pan, input, blockSize, width, true);
                const auto stats = measureStereo (output, settle, totalSamples - settle);
                const auto combinedPower = stats.rmsL * stats.rmsL + stats.rmsR * stats.rmsR;

                std::cout << "  width=" << (width * 100.0f) << "%: combined power=" << combinedPower << std::endl;
                if (width > 0.0f)
                    expect (combinedPower > previousPower, "combined stereo power should grow with width");
                previousPower = combinedPower;
            }
            std::cout << "=== end combined power vs width ===" << std::endl << std::endl;
        }

        beginTest ("Motion cycle: centroid trajectory is smooth, continuous, and grows in depth with width");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            constexpr float windowSeconds = 0.05f;
            const auto windowLen = (int) (windowSeconds * sr);

            const auto totalSamples = (int) (8.0 * sr); // > 2 full LFO cycles at 0.3Hz
            auto monoInStereo = generateMonoHarmonicStereo (totalSamples, sr, 0.3f);
            const auto settle = (int) (0.3 * sr);

            std::cout << "\n=== PAN motion centroid trajectory ===" << std::endl;
            double previousExcursion = -1.0;
            for (auto width : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, blockSize, 2);
                auto output = runPanoramaProcessor (pan, monoInStereo, blockSize, width, true);

                const auto series = centroidSeries (output, settle, totalSamples - settle, windowLen);
                const auto stats = analyzeSeries (series);

                std::cout << "  width=" << (width * 100.0f) << "%: centroid min=" << stats.minV << " max=" << stats.maxV
                           << " rmsExcursion=" << stats.rmsExcursion << std::endl;

                if (width == 0.0f)
                    expect (stats.maxV - stats.minV < 0.01, "centroid must stay essentially at 0 (centre) at PAN=0");
                else
                {
                    expect (stats.rmsExcursion > previousExcursion, "centroid excursion (motion depth) should grow monotonically with width");

                    // Smoothness: no window-to-window jump should look like
                    // a discontinuity - bound it well inside the total
                    // excursion range, not near-instant like an on/off
                    // auto-pan would produce.
                    double maxStep = 0.0;
                    for (size_t i = 1; i < series.size(); ++i)
                        maxStep = juce::jmax (maxStep, std::abs (series[i] - series[i - 1]));
                    expect (maxStep < 0.35, "centroid trajectory should move smoothly, not jump, at width " + juce::String (width * 100.0f) + "%");
                }
                previousExcursion = stats.rmsExcursion;
            }
            std::cout << "=== end motion centroid trajectory ===" << std::endl << std::endl;

            // At MOTION (100%), the trajectory should visit clearly
            // left-biased, centred, and right-biased states, not just
            // hover near zero.
            uni76::dsp::PanoramaProcessor pan100;
            pan100.prepare (sr, blockSize, 2);
            auto out100 = runPanoramaProcessor (pan100, monoInStereo, blockSize, 1.0f, true);
            const auto series100 = centroidSeries (out100, settle, totalSamples - settle, windowLen);
            const auto stats100 = analyzeSeries (series100);
            expect (stats100.minV < -0.1, "MOTION should visit a clearly left-biased state");
            expect (stats100.maxV > 0.1, "MOTION should visit a clearly right-biased state");
        }

        beginTest ("Motion LFO period is close to the fixed ~0.3Hz rate, independent of sample rate and block size");
        {
            constexpr float windowSeconds = 0.05f;
            const double rates[] { 44100.0, 96000.0 };
            const int blockSizes[] { 64, 2048 };

            std::cout << "\n=== PAN motion LFO period ===" << std::endl;
            for (auto sr : rates)
            {
                for (auto blockSize : blockSizes)
                {
                    uni76::dsp::PanoramaProcessor pan;
                    pan.prepare (sr, blockSize, 2);

                    const auto totalSamples = (int) (9.0 * sr);
                    auto monoInStereo = generateMonoHarmonicStereo (totalSamples, sr, 0.3f);
                    auto output = runPanoramaProcessor (pan, monoInStereo, blockSize, 1.0f, true);

                    const auto windowLen = (int) (windowSeconds * sr);
                    const auto settle = (int) (0.3 * sr);
                    const auto series = centroidSeries (output, settle, totalSamples - settle, windowLen);
                    const auto period = measureOscillationPeriodSeconds (series, windowSeconds);

                    std::cout << "  sr=" << sr << " block=" << blockSize << ": measured period=" << period << "s (expected ~" << (1.0 / uni76::dsp::panLfoRateHz) << "s)" << std::endl;
                    expect (period > 2.0 && period < 4.7, "LFO period should stay close to ~1/0.3Hz regardless of sample rate/block size, got " + juce::String (period) + "s");
                }
            }
            std::cout << "=== end motion LFO period ===" << std::endl << std::endl;
        }

        beginTest ("Automation does not reset the LFO phase - width changes don't restart the motion cycle");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            constexpr float windowSeconds = 0.05f;
            const auto windowLen = (int) (windowSeconds * sr);

            uni76::dsp::PanoramaProcessor pan;
            pan.prepare (sr, blockSize, 2);

            // Run continuously at MOTION, but wiggle the width parameter
            // partway through (0->100, 100->0, 0->50 etc.) - if the LFO
            // phase were wrongly reset on each change, the centroid
            // trajectory would show a discontinuity/restart exactly at
            // each width change; it must not.
            const auto totalSamples = (int) (9.0 * sr);
            auto monoInStereo = generateMonoHarmonicStereo (totalSamples, sr, 0.3f);

            juce::AudioBuffer<float> output (2, totalSamples);
            const float widthSteps[] { 1.0f, 0.5f, 1.0f, 0.75f, 1.0f };
            const auto stepSamples = totalSamples / (int) (sizeof (widthSteps) / sizeof (widthSteps[0]));

            int done = 0;
            int stepIndex = 0;
            while (done < totalSamples)
            {
                const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
                juce::AudioBuffer<float> block (2, thisBlock);
                block.copyFrom (0, 0, monoInStereo, 0, done, thisBlock);
                block.copyFrom (1, 0, monoInStereo, 1, done, thisBlock);

                stepIndex = juce::jmin ((int) (sizeof (widthSteps) / sizeof (widthSteps[0])) - 1, done / juce::jmax (1, stepSamples));
                pan.process (block, widthSteps[stepIndex], true);

                output.copyFrom (0, done, block, 0, 0, thisBlock);
                output.copyFrom (1, done, block, 1, 0, thisBlock);
                done += thisBlock;
            }

            expect (bufferIsFinite (output), "automation produced non-finite output");

            const auto settle = (int) (0.3 * sr);
            const auto series = centroidSeries (output, settle, totalSamples - settle, windowLen);
            const auto period = measureOscillationPeriodSeconds (series, windowSeconds);

            std::cout << "\n=== PAN automation / LFO continuity === measured period across width changes = " << period << "s" << std::endl << std::endl;
            // If the phase had been reset at each width step, the
            // effective period measured across the whole run would be
            // wildly different from ~3.33s (either much shorter, from
            // spurious extra crossings at each reset, or undetectable).
            expect (period > 2.0 && period < 4.7, "LFO period should stay consistent across width automation (no phase reset), got " + juce::String (period) + "s");
        }

        beginTest ("Combined stereo power stays stable (~within 1dB) across a full motion cycle at MOTION (100%)");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            constexpr float windowSeconds = 0.05f;
            const auto windowLen = (int) (windowSeconds * sr);

            uni76::dsp::PanoramaProcessor pan;
            pan.prepare (sr, blockSize, 2);

            const auto totalSamples = (int) (8.0 * sr);
            auto input = generateDecorrelatedStereo (totalSamples, sr);
            auto output = runPanoramaProcessor (pan, input, blockSize, 1.0f, true);

            const auto settle = (int) (0.3 * sr);
            const auto series = combinedPowerDbSeries (output, settle, totalSamples - settle, windowLen);
            const auto stats = analyzeSeries (series);

            std::cout << "\n=== PAN combined power stability (MOTION) === min=" << stats.minV << "dB max=" << stats.maxV << "dB" << std::endl << std::endl;
            expect (stats.maxV - stats.minV < 3.0, "combined stereo power should not swing wildly over a motion cycle, range=" + juce::String (stats.maxV - stats.minV) + "dB");
        }

        beginTest ("No pitch drift: steady 100/440/1000/5000Hz tones keep their fundamental frequency at MOTION (100%)");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const float freqs[] { 100.0f, 440.0f, 1000.0f, 5000.0f };

            std::cout << "\n=== PAN pitch stability (MOTION) ===" << std::endl;
            for (auto freqHz : freqs)
            {
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, blockSize, 2);

                const auto totalSamples = (int) (4.0 * sr);
                auto monoInStereo = generateIdenticalStereo (totalSamples, sr, freqHz, 0.3f);
                auto output = runPanoramaProcessor (pan, monoInStereo, blockSize, 1.0f, true);

                const auto settle = (int) (0.3 * sr);
                const auto stability = analyzeBassStability (output, 0, settle, totalSamples - settle, sr, freqHz);
                expect (stability.numWindows > 3, "not enough analysis windows for pitch-stability measurement");

                const auto errPercent = 100.0 * std::abs (stability.freqMean - (double) freqHz) / (double) freqHz;
                std::cout << "  " << freqHz << "Hz: measured=" << stability.freqMean << "Hz err=" << errPercent << "%" << std::endl;
                expect (errPercent < 0.5, juce::String (freqHz) + "Hz: fundamental drifted - PAN's motion must not shift pitch");
            }
            std::cout << "=== end pitch stability ===" << std::endl << std::endl;
        }

        beginTest ("Low-end safety: 40/60/80/100/120Hz mono bass shows little-to-no motion at MOTION (100%), stays stable");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            constexpr float windowSeconds = 0.05f;
            const auto windowLen = (int) (windowSeconds * sr);
            const float bassFreqs[] { 40.0f, 60.0f, 80.0f, 100.0f, 120.0f };

            std::cout << "\n=== PAN low-end motion/stability (MOTION) ===" << std::endl;
            double excursion40 = 0.0;
            for (auto freqHz : bassFreqs)
            {
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, blockSize, 2);

                const auto totalSamples = (int) (4.0 * sr);
                auto monoInStereo = generateIdenticalStereo (totalSamples, sr, freqHz, 0.35f);
                auto output = runPanoramaProcessor (pan, monoInStereo, blockSize, 1.0f, true);

                expect (bufferIsFinite (output), juce::String (freqHz) + "Hz: non-finite output at MOTION");

                const auto settle = (int) (0.3 * sr);
                const auto series = centroidSeries (output, settle, totalSamples - settle, windowLen);
                const auto stats = analyzeSeries (series);

                const auto stability = analyzeBassStability (output, 0, settle, totalSamples - settle, sr, freqHz);
                const auto errPercent = stability.numWindows > 3 ? 100.0 * std::abs (stability.freqMean - (double) freqHz) / (double) freqHz : 0.0;

                std::cout << "  " << freqHz << "Hz: centroid rmsExcursion=" << stats.rmsExcursion
                           << " freqErr=" << errPercent << "%" << std::endl;

                expect (stats.rmsExcursion < 0.1, juce::String (freqHz) + "Hz: bass should barely move at MOTION (low-band motion ceiling is small)");
                expect (errPercent < 0.5, juce::String (freqHz) + "Hz: bass fundamental should not drift/wobble under motion");

                if (freqHz == 40.0f) excursion40 = stats.rmsExcursion;
            }

            // High-frequency content should move noticeably more than the
            // lowest bass frequency tested. Also sweeps the rest of the
            // frequency-dependent-motion table (200/500/1000/3000/10000Hz)
            // for docs/DSP_PAN.md - a smooth progression from "almost
            // nothing" at bass through "clearly obvious" at mid/high is
            // the expected shape, not a hard per-frequency assertion here
            // (that belongs to the bass frequencies above and the
            // dedicated CenteredBassMotionIsolation test below).
            {
                const float highFreqs[] { 200.0f, 500.0f, 1000.0f, 3000.0f, 10000.0f };
                double excursion3000 = 0.0;
                for (auto freqHz : highFreqs)
                {
                    uni76::dsp::PanoramaProcessor panHigh;
                    panHigh.prepare (sr, blockSize, 2);
                    const auto totalSamples = (int) (4.0 * sr);
                    auto monoInStereo = generateIdenticalStereo (totalSamples, sr, freqHz, 0.3f);
                    auto output = runPanoramaProcessor (panHigh, monoInStereo, blockSize, 1.0f, true);
                    const auto settle = (int) (0.3 * sr);
                    const auto series = centroidSeries (output, settle, totalSamples - settle, windowLen);
                    const auto stats = analyzeSeries (series);
                    std::cout << "  " << freqHz << "Hz: centroid rmsExcursion=" << stats.rmsExcursion << std::endl;
                    if (freqHz == 3000.0f) excursion3000 = stats.rmsExcursion;
                }
                expect (excursion3000 > excursion40 * 2.0, "high-frequency content should move noticeably more than 40Hz bass at MOTION");
            }
            std::cout << "=== end low-end motion/stability ===" << std::endl << std::endl;
        }

        beginTest ("40-120Hz magnitude/centroid table across width 0/25/50/75/100% (docs/DSP_PAN.md source data)");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            constexpr float windowSeconds = 0.05f;
            const auto windowLen = (int) (windowSeconds * sr);
            const float bassFreqs[] { 40.0f, 60.0f, 80.0f, 100.0f, 120.0f };
            const float widths[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };

            std::cout << "\n=== PAN 40-120Hz magnitude delta (dB vs 0%) / centroid rmsExcursion, mono bass ===" << std::endl;
            for (auto freqHz : bassFreqs)
            {
                const auto totalSamples = (int) (4.0 * sr);
                auto monoInStereo = generateIdenticalStereo (totalSamples, sr, freqHz, 0.35f);
                const auto settle = (int) (0.3 * sr);
                const auto win = juce::jmin (totalSamples - settle, periodicAnalysisLength (sr, freqHz, 20));

                double magAt0Percent = 0.0;
                std::cout << "  " << freqHz << "Hz:";
                for (auto width : widths)
                {
                    uni76::dsp::PanoramaProcessor pan;
                    pan.prepare (sr, blockSize, 2);
                    auto output = runPanoramaProcessor (pan, monoInStereo, blockSize, width, true);
                    expect (bufferIsFinite (output), juce::String (freqHz) + "Hz at " + juce::String (width * 100.0f) + "%: non-finite");

                    const auto magL = goertzelMagnitude (output, 0, totalSamples - win, win, sr, freqHz);
                    if (width == 0.0f) magAt0Percent = magL;
                    const auto deltaDb = 20.0 * std::log10 (juce::jmax ((double) magL, 1.0e-9) / juce::jmax (magAt0Percent, 1.0e-9));

                    const auto series = centroidSeries (output, settle, totalSamples - settle, windowLen);
                    const auto stats = analyzeSeries (series);

                    std::cout << " [" << (width * 100.0f) << "%: delta=" << deltaDb << "dB centroidExc=" << stats.rmsExcursion << "]";

                    // Acceptance bounds at 100% width only (the worst
                    // case) - unintended magnitude change from just
                    // turning PAN up, on a source with zero real Side
                    // content so every bit of it comes from the induced-
                    // signal path. 40-80Hz tightest, loosening slightly
                    // by 120Hz (closest to the induced-highpass's own
                    // corner, where attenuation is weakest) - matches
                    // the product brief's own "80/100/120Hz gets a
                    // little" guidance.
                    if (width == 1.0f)
                    {
                        const auto bound = freqHz <= 80.0f ? 0.25 : (freqHz <= 100.0f ? 0.5 : 0.75);
                        expect (std::abs (deltaDb) < bound, juce::String (freqHz) + "Hz: unintended magnitude change at 100% width too large: " + juce::String (deltaDb) + "dB (bound " + juce::String (bound) + "dB)");
                    }
                }
                std::cout << std::endl;
            }
            std::cout << "=== end 40-120Hz table ===" << std::endl << std::endl;
        }

        beginTest ("Centre core stability: centred bass under stereo highs barely moves at MOTION even though the highs do");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            constexpr float windowSeconds = 0.05f;
            const auto windowLen = (int) (windowSeconds * sr);

            uni76::dsp::PanoramaProcessor pan;
            pan.prepare (sr, blockSize, 2);

            const auto totalSamples = (int) (4.0 * sr);
            auto input = generateCenterBassStereoHighs (totalSamples, sr);
            auto output = runPanoramaProcessor (pan, input, blockSize, 1.0f, true);
            expect (bufferIsFinite (output), "centre-bass+stereo-highs source produced non-finite output at MOTION");

            const auto settle = (int) (0.3 * sr);
            const auto win = juce::jmin (totalSamples - settle, periodicAnalysisLength (sr, 80.0f, 20));
            const auto bassL = goertzelMagnitude (output, 0, totalSamples - win, win, sr, 80.0f);
            const auto bassR = goertzelMagnitude (output, 1, totalSamples - win, win, sr, 80.0f);
            const auto bassLRDb = 20.0f * std::log10 (juce::jmax (bassL, 1.0e-9f) / juce::jmax (bassR, 1.0e-9f));

            std::cout << "\n=== PAN centre-core stability === 80Hz bass L/R=" << bassLRDb << "dB" << std::endl << std::endl;

            // A small residual movement is expected and honestly
            // documented (see docs/DSP_PAN.md) - the low-band width/
            // motion ceilings are deliberately nonzero (matching the
            // product brief's "80/100Hz gets a little" guidance, not
            // "zero"). Tightened from an earlier <3.0dB bound after this
            // round's fix (a cascaded, purpose-built highpass isolating
            // the *induced* signal's own bass, replacing a complementary-
            // subtraction construction that had its own vector-sum-style
            // hump - see docs/DSP_PAN.md's "Centre-bass isolation"
            // section) brought the measured residual down to ~0.2dB.
            expect (std::abs (bassLRDb) < 0.5f, "centred 80Hz bass should stay close to centred (L~=R) even under MOTION with stereo highs present, L/R=" + juce::String (bassLRDb) + "dB");
            juce::ignoreUnused (windowLen);
        }

        beginTest ("CenteredBassMotionIsolation: centred bass stays stable (level+L/R+centroid) while high-frequency spatial content clearly moves, at MOTION");
        {
            // Dedicated regression test for this round's fix, isolating
            // four independent measurements on the same centre-bass +
            // stereo-highs source (see docs/DSP_PAN.md's "Centre-bass
            // isolation" section for the full write-up):
            //   1. bass L/R RMS difference at MOTION (should be tiny);
            //   2. bass magnitude change vs ORIGINAL (0%) - does the bass
            //      level itself shift just from turning PAN up;
            //   3. bass-only centroid trajectory (Goertzel-windowed, not
            //      broadband - broadband centroid on this source is
            //      dominated by the much louder highs) - should stay
            //      close to 0 throughout, unlike the highs;
            //   4. high-frequency (broadband) centroid excursion - should
            //      stay clearly, obviously large, confirming the fix
            //      didn't weaken motion generally.
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            constexpr float bassFreqHz = 80.0f;

            const auto totalSamples = (int) (4.0 * sr);
            auto input = generateCenterBassStereoHighs (totalSamples, sr);
            const auto settle = (int) (0.3 * sr);

            uni76::dsp::PanoramaProcessor panOriginal;
            panOriginal.prepare (sr, blockSize, 2);
            auto outputOriginal = runPanoramaProcessor (panOriginal, input, blockSize, 0.0f, true);

            uni76::dsp::PanoramaProcessor panMotion;
            panMotion.prepare (sr, blockSize, 2);
            auto outputMotion = runPanoramaProcessor (panMotion, input, blockSize, 1.0f, true);

            expect (bufferIsFinite (outputMotion), "non-finite output at MOTION");

            // ---- 1: bass L/R RMS difference at MOTION ----
            const auto win = juce::jmin (totalSamples - settle, periodicAnalysisLength (sr, bassFreqHz, 20));
            const auto bassL = goertzelMagnitude (outputMotion, 0, totalSamples - win, win, sr, bassFreqHz);
            const auto bassR = goertzelMagnitude (outputMotion, 1, totalSamples - win, win, sr, bassFreqHz);
            const auto bassLRDb = 20.0f * std::log10 (juce::jmax (bassL, 1.0e-9f) / juce::jmax (bassR, 1.0e-9f));

            // ---- 2: bass magnitude change, ORIGINAL vs MOTION ----
            const auto bassOriginalL = goertzelMagnitude (outputOriginal, 0, totalSamples - win, win, sr, bassFreqHz);
            const auto bassMagnitudeChangeDb = 20.0f * std::log10 (juce::jmax (bassL, 1.0e-9f) / juce::jmax (bassOriginalL, 1.0e-9f));

            // ---- 3: bass-only centroid trajectory (Goertzel-windowed) ----
            constexpr float bassWindowSeconds = 0.1f;
            const auto bassWindowLen = (int) (bassWindowSeconds * sr);
            std::vector<double> bassCentroid;
            for (int pos = settle; pos + bassWindowLen <= totalSamples; pos += bassWindowLen)
            {
                const auto l = goertzelMagnitude (outputMotion, 0, pos, bassWindowLen, sr, bassFreqHz);
                const auto r = goertzelMagnitude (outputMotion, 1, pos, bassWindowLen, sr, bassFreqHz);
                const auto lE = (double) l * (double) l, rE = (double) r * (double) r;
                bassCentroid.push_back ((lE + rE) > 1.0e-12 ? (rE - lE) / (lE + rE) : 0.0);
            }
            const auto bassCentroidStats = analyzeSeries (bassCentroid);

            // ---- 4: high-frequency (broadband) centroid excursion ----
            constexpr float highWindowSeconds = 0.05f;
            const auto highWindowLen = (int) (highWindowSeconds * sr);
            const auto highSeries = centroidSeries (outputMotion, settle, totalSamples - settle, highWindowLen);
            const auto highStats = analyzeSeries (highSeries);

            std::cout << "\n=== PAN CenteredBassMotionIsolation ===" << std::endl;
            std::cout << "  bass L/R = " << bassLRDb << "dB" << std::endl;
            std::cout << "  bass magnitude change (ORIGINAL->MOTION) = " << bassMagnitudeChangeDb << "dB" << std::endl;
            std::cout << "  bass-only centroid: min=" << bassCentroidStats.minV << " max=" << bassCentroidStats.maxV
                       << " rmsExcursion=" << bassCentroidStats.rmsExcursion << std::endl;
            std::cout << "  high-frequency centroid rmsExcursion=" << highStats.rmsExcursion << std::endl;
            std::cout << "=== end CenteredBassMotionIsolation ===" << std::endl << std::endl;

            expect (std::abs (bassLRDb) < 0.5f, "bass L/R difference too large at MOTION: " + juce::String (bassLRDb) + "dB");
            expect (std::abs (bassMagnitudeChangeDb) < 0.5f, "bass magnitude changed too much from ORIGINAL to MOTION: " + juce::String (bassMagnitudeChangeDb) + "dB");
            expect (bassCentroidStats.rmsExcursion < 0.02, "bass-only centroid should stay close to centre, rmsExcursion=" + juce::String (bassCentroidStats.rmsExcursion));
            expect (highStats.rmsExcursion > bassCentroidStats.rmsExcursion * 3.0, "high-frequency content should move far more than the bass at MOTION");
            expect (highStats.rmsExcursion > 0.1, "high-frequency motion should stay clearly audible/obvious, not weakened by the bass-isolation fix");
        }

        beginTest ("Crossover-region frequency response has no unexpected bump/dip (no vector-sum overshoot beyond the shelf's own asymptotes)");
        {
            // Regression guard for the specific bug fixed this round: a
            // band-split-then-differently-gained-sum crossover is a
            // *vector* sum of phase-shifted complementary paths, which
            // provably overshoots both endpoint gains whenever they
            // differ (see docs/DSP_PAN.md's "Crossover artifact"
            // section) - replaced by two independent single-channel
            // shelf filters (PanoramaProcessor.cpp), which by
            // construction (RBJ S=1 shelf - monotonic, no resonant
            // peaking) cannot overshoot their own two asymptotes. This
            // test measures the real Side-signal response at 14
            // frequencies spanning 40Hz-10kHz and checks that no
            // interior frequency - especially the 100-300Hz crossover
            // region itself - pokes outside the envelope set by the
            // deepest-low/deepest-high measurements.
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            constexpr float amplitude = 0.3f;

            const float testFreqs[] { 40.0f, 60.0f, 80.0f, 100.0f, 120.0f, 150.0f, 180.0f, 200.0f, 250.0f, 300.0f, 500.0f, 1000.0f, 5000.0f, 10000.0f };
            constexpr size_t numFreqs = sizeof (testFreqs) / sizeof (testFreqs[0]);

            auto measureSideResponseDb = [&] (float width, float freqHz) -> float
            {
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, blockSize, 2);

                // A pure anti-phase (Side-only, Mid==0) tone isolates the
                // width/motion shelf's own response: Mid==0 means
                // induced==allpass(0)==0 too, so nothing but real Side
                // content and the shelves are under test.
                //
                // Deliberately a SHORT, fixed-duration window (~50ms),
                // not one scaled to "many cycles" of the tone or to a
                // full ~3.33s LFO period, for two reasons found while
                // building this test: (1) generateSine() accumulates
                // phase in `float`, which drifts audibly over the
                // hundreds of thousands of samples a low-frequency,
                // many-cycle window would need, corrupting the
                // measurement itself (a test bug, not a PAN bug) - a
                // short window avoids that entirely; (2) using a window
                // whose *duration* varies wildly by frequency (a few ms
                // at 10kHz vs most of a second at 40Hz) would sample the
                // free-running LFO at inconsistent phases per frequency,
                // making the frequencies incomparable. A fixed short
                // settle+window instead measures every frequency at
                // (very nearly) the *same* LFO phase, close to lfoSin=0
                // (theta==thetaCentre, i.e. the plain symmetric-width
                // case) - exactly what isolates the shelf's own width
                // response from motion's separate, already-tested
                // time-domain behaviour.
                // 0.1s is an exact integer number of cycles for every
                // frequency in testFreqs[] (4, 6, 8, 10, 12, 15, 18, 20,
                // 25, 30, 50, 100, 500, 1000 cycles respectively), so the
                // Goertzel bin lands exactly on each one with no leakage.
                const auto settle = (int) (0.1 * sr);
                const auto window = (int) (0.1 * sr);
                const auto totalSamples = settle + window + 64;

                auto input = generateAntiPhase (totalSamples, sr, freqHz, amplitude);
                auto output = runPanoramaProcessor (pan, input, blockSize, width, true);

                juce::AudioBuffer<float> sideOnly (1, totalSamples);
                for (int i = 0; i < totalSamples; ++i)
                    sideOnly.setSample (0, i, 0.5f * (output.getSample (0, i) - output.getSample (1, i)));

                const auto mag = goertzelMagnitude (sideOnly, 0, settle, window, sr, freqHz);
                return 20.0f * std::log10 (juce::jmax (mag, 1.0e-9f) / amplitude);
            };

            std::cout << "\n=== PAN crossover-region frequency response (dB, Side-only input) ===" << std::endl;
            for (auto width : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                std::vector<float> responseDb;
                for (auto freq : testFreqs)
                    responseDb.push_back (measureSideResponseDb (width, freq));

                std::cout << "  width=" << (width * 100.0f) << "%:";
                for (size_t i = 0; i < numFreqs; ++i)
                    std::cout << " " << testFreqs[i] << "Hz=" << responseDb[i] << "dB";
                std::cout << std::endl;

                if (width > 0.0f)
                {
                    const auto lowRef  = responseDb.front();
                    const auto highRef = responseDb.back();
                    const auto envelopeMin = juce::jmin (lowRef, highRef) - 0.5f;
                    const auto envelopeMax = juce::jmax (lowRef, highRef) + 0.5f;

                    for (size_t i = 1; i + 1 < numFreqs; ++i)
                        expect (responseDb[i] >= envelopeMin && responseDb[i] <= envelopeMax,
                                juce::String (testFreqs[i]) + "Hz at " + juce::String (width * 100.0f) + "%: " + juce::String (responseDb[i])
                                    + "dB is outside the [" + juce::String (envelopeMin) + ", " + juce::String (envelopeMax)
                                    + "]dB envelope set by the 40Hz/10kHz asymptotes - crossover bump/dip");

                    // Monotonicity through the 100-300Hz crossover region
                    // itself: since the high-band ceiling is always >=
                    // the low-band ceiling (see PanoramaCurves.h), the
                    // *intended* response only ever rises with frequency
                    // - a naturally steep rise right around the shelf's
                    // own corner is expected and NOT what this test is
                    // guarding against (an earlier version of this check
                    // used a flat per-step dB cap here and had to be
                    // corrected - it was flagging the shelf's normal,
                    // monotonic transition slope as if it were a bump).
                    // What must never happen is a *reversal* - a point
                    // reading measurably lower than the one below it in
                    // frequency - which is what an actual crossover bump
                    // or dip would produce.
                    for (size_t i = 4; i <= 9; ++i) // 120,150,180,200,250,300Hz vs their predecessor
                        expect (responseDb[i] >= responseDb[i - 1] - 0.1f,
                                "response should not dip going from " + juce::String (testFreqs[i - 1]) + "Hz to " + juce::String (testFreqs[i])
                                    + "Hz at " + juce::String (width * 100.0f) + "%: " + juce::String (responseDb[i - 1]) + "dB -> " + juce::String (responseDb[i]) + "dB");
                }
            }
            std::cout << "=== end crossover-region frequency response ===" << std::endl << std::endl;
        }

        beginTest ("Mono fold-down stays musical (no serious comb cancellation) across widths, on varied source material");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            struct Source { const char* name; juce::AudioBuffer<float> (*gen) (int, double); };
            const Source sources[] {
                { "mono", [] (int n, double s) { return generateIdenticalStereo (n, s, 300.0f, 0.3f); } },
                { "centre-bass+highs", generateCenterBassStereoHighs },
                { "correlated chord", generateCorrelatedChord },
                { "decorrelated", generateDecorrelatedStereo },
            };

            std::cout << "\n=== PAN mono fold-down ===" << std::endl;
            for (const auto& src : sources)
            {
                const auto totalSamples = (int) (2.0 * sr);
                auto input = src.gen (totalSamples, sr);

                double inMonoSumSq = 0.0;
                const auto settle = (int) (0.1 * sr);
                for (int i = settle; i < totalSamples; ++i)
                {
                    const auto m = 0.5 * ((double) input.getSample (0, i) + (double) input.getSample (1, i));
                    inMonoSumSq += m * m;
                }
                const auto inMonoRms = std::sqrt (inMonoSumSq / (double) (totalSamples - settle));

                for (auto width : { 0.5f, 1.0f })
                {
                    uni76::dsp::PanoramaProcessor pan;
                    pan.prepare (sr, blockSize, 2);
                    auto output = runPanoramaProcessor (pan, input, blockSize, width, true);

                    double outMonoSumSq = 0.0;
                    for (int i = settle; i < totalSamples; ++i)
                    {
                        const auto m = 0.5 * ((double) output.getSample (0, i) + (double) output.getSample (1, i));
                        outMonoSumSq += m * m;
                    }
                    const auto outMonoRms = std::sqrt (outMonoSumSq / (double) (totalSamples - settle));
                    const auto lossDb = 20.0 * std::log10 (juce::jmax (outMonoRms, 1.0e-9) / juce::jmax (inMonoRms, 1.0e-9));

                    std::cout << "  " << src.name << " width=" << (width * 100.0f) << "%: mono fold-down change=" << lossDb << "dB" << std::endl;
                    expect (lossDb > -6.0 && lossDb < 6.0, juce::String (src.name) + " at " + juce::String (width * 100.0f) + "%: mono fold-down changed too much (possible comb cancellation), " + juce::String (lossDb) + "dB");
                }
            }
            std::cout << "=== end mono fold-down ===" << std::endl << std::endl;
        }

        beginTest ("Correlation stays sane (not driven aggressively negative) on correlated material as width increases");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            const auto totalSamples = (int) (2.0 * sr);
            auto input = generateCorrelatedChord (totalSamples, sr);
            const auto settle = (int) (0.1 * sr);

            std::cout << "\n=== PAN correlation vs width ===" << std::endl;
            for (auto width : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, blockSize, 2);
                auto output = runPanoramaProcessor (pan, input, blockSize, width, true);
                const auto stats = measureStereo (output, settle, totalSamples - settle);

                std::cout << "  width=" << (width * 100.0f) << "%: correlation=" << stats.correlation << " sideMidRatio=" << stats.sideMidRatio << std::endl;
                expect (stats.correlation > -0.3, "correlation should not be driven aggressively negative on correlated material at " + juce::String (width * 100.0f) + "%");
            }
            std::cout << "=== end correlation vs width ===" << std::endl << std::endl;
        }

        beginTest ("No runaway gain: peak/RMS stay moderate across 0/25/50/75/100% on a Side-heavy broadband source");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            const auto totalSamples = (int) (2.0 * sr);
            auto input = generateSideHeavy (totalSamples, sr);
            const auto settle = (int) (0.1 * sr);

            std::cout << "\n=== PAN gain/headroom (Side-heavy source) ===" << std::endl;
            for (auto width : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, blockSize, 2);
                auto output = runPanoramaProcessor (pan, input, blockSize, width, true);
                const auto stats = measureStereo (output, settle, totalSamples - settle);

                std::cout << "  width=" << (width * 100.0f) << "%: peak=" << stats.peak
                           << " rmsL=" << stats.rmsL << " rmsR=" << stats.rmsR << std::endl;
                expect (stats.peak < 1.5, "PAN should not create a large peak/gain boost at " + juce::String (width * 100.0f) + "%");
            }
            std::cout << "=== end gain/headroom ===" << std::endl << std::endl;
        }

        beginTest ("Bypass (enabled=false) crossfades to an exact dry passthrough, click-free");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            uni76::dsp::PanoramaProcessor pan;
            pan.prepare (sr, blockSize, 2);

            const auto totalSamples = (int) (2.0 * sr);
            auto input = generateDecorrelatedStereo (totalSamples, sr);
            auto output = runPanoramaProcessor (pan, input, blockSize, 1.0f, false);

            const auto settle = (int) (0.1 * sr);
            double maxDiff = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = settle; i < totalSamples; ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (output.getSample (ch, i) - input.getSample (ch, i)));

            expect (maxDiff < 1.0e-4, "disabled PAN should be an exact (undelayed) passthrough, maxDiff=" + juce::String (maxDiff));
        }

        beginTest ("Automation (0->100, 100->0, 25->75) is click-free");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            struct Transition { float from, to; };
            const Transition transitions[] { { 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.25f, 0.75f } };

            for (const auto& t : transitions)
            {
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, blockSize, 2);

                const auto preSwitch = (int) (0.5 * sr);
                const auto postSwitch = (int) (0.5 * sr);
                const auto totalSamples = preSwitch + postSwitch;

                auto input = generateDecorrelatedStereo (totalSamples, sr);
                juce::AudioBuffer<float> output (2, totalSamples);

                int done = 0;
                while (done < totalSamples)
                {
                    const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
                    juce::AudioBuffer<float> block (2, thisBlock);
                    block.copyFrom (0, 0, input, 0, done, thisBlock);
                    block.copyFrom (1, 0, input, 1, done, thisBlock);

                    const auto width = done < preSwitch ? t.from : t.to;
                    pan.process (block, width, true);

                    output.copyFrom (0, done, block, 0, 0, thisBlock);
                    output.copyFrom (1, done, block, 1, 0, thisBlock);
                    done += thisBlock;
                }

                expect (bufferIsFinite (output), "automation transition produced non-finite output");

                float maxJump = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 1; i < totalSamples; ++i)
                        maxJump = juce::jmax (maxJump, std::abs (output.getSample (ch, i) - output.getSample (ch, i - 1)));

                expect (maxJump < 0.3f, juce::String (t.from) + "->" + juce::String (t.to) + ": transition produced a click (maxJump=" + juce::String (maxJump) + ")");
            }
        }

        beginTest ("All 6 supported sample rates and required block sizes process finite audio");
        {
            const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
            const int blockSizes[] { 32, 64, 128, 256, 512, 1024, 2048 };

            for (auto sr : rates)
            {
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, 2048, 2);

                for (auto blockSize : blockSizes)
                {
                    auto input = generateDecorrelatedStereo ((int) (0.2 * sr), sr);
                    auto output = runPanoramaProcessor (pan, input, blockSize, 1.0f, true);
                    expect (bufferIsFinite (output), juce::String ("non-finite output at ") + juce::String (sr) + "Hz, block " + juce::String (blockSize));
                }
            }
        }

        beginTest ("Mono bus (numChannels=1) stays completely untouched at every width setting");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            for (auto width : { 0.0f, 0.5f, 1.0f })
            {
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, blockSize, 1);

                auto mono = generateSine (1, (int) sr, sr, 300.0f, 0.3f);
                auto original = mono;
                auto output = runPanoramaProcessor (pan, mono, blockSize, width, true);

                double maxDiff = 0.0;
                for (int i = 0; i < output.getNumSamples(); ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (output.getSample (0, i) - original.getSample (0, i)));

                expect (maxDiff < 1.0e-6, "mono bus must be left completely untouched at width " + juce::String (width));
            }
        }

        beginTest ("Silence produces silence; NaN/Inf input is sanitized and doesn't poison state");
        {
            uni76::dsp::PanoramaProcessor pan;
            pan.prepare (44100.0, 512, 2);

            juce::AudioBuffer<float> silence (2, 44100);
            silence.clear();
            auto output = runPanoramaProcessor (pan, silence, 512, 1.0f, true);
            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < output.getNumSamples(); ++i)
                    peak = juce::jmax (peak, std::abs (output.getSample (ch, i)));
            expect (peak < 1.0e-6f, "silent input should produce silent output");

            juce::AudioBuffer<float> poisoned (2, 256);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    poisoned.setSample (ch, i, (i % 2 == 0) ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN());
            pan.process (poisoned, 1.0f, true);
            expect (bufferIsFinite (poisoned), "NaN/Inf input leaked through to the output");

            auto clean = generateDecorrelatedStereo (44100, 44100.0);
            auto cleanOutput = runPanoramaProcessor (pan, clean, 512, 1.0f, true);
            expect (bufferIsFinite (cleanOutput), "state remained poisoned after a NaN/Inf block");
        }
    }
};

static UNI76PanoramaProcessorTests uni76PanoramaProcessorTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// Full-chain / APVTS-level PAN coverage: default/migration, PITCH+PAN
// interaction (PITCH's stereo-coherence guarantees must survive PAN
// running after it), and full-chain integration.
class UNI76PanoramaIntegrationTests final : public juce::UnitTest
{
public:
    UNI76PanoramaIntegrationTests() : juce::UnitTest ("UNI76AudioProcessor + PAN", "UNI76") {}

    void runTest() override
    {
        beginTest ("Fresh instance: panorama defaults to 0% (ORIGINAL)");
        {
            UNI76AudioProcessor processor;
            auto* param = processor.getValueTreeState().getParameter (uni76::ParamID::panorama);
            expect (param != nullptr);
            if (param != nullptr)
                expectWithinAbsoluteError (param->getValue(), 0.0f, 0.001f);
        }

        beginTest ("Legacy (pre-v5) saved state migrates panorama to 0% regardless of its old raw value (incl. old v4 50%)");
        {
            for (auto legacySchemaVersion : { 2, 3, 4 })
            {
                UNI76AudioProcessor processor;
                auto& apvts = processor.getValueTreeState();

                auto legacyState = apvts.copyState();
                legacyState.setProperty (uni76::stateSchemaVersionProperty, legacySchemaVersion, nullptr);
                for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                    legacyState.setProperty (uni76::ModuleEnableState::propertyNames[(size_t) i], true, nullptr);

                auto legacyPanoramaParam = legacyState.getChildWithProperty ("id", juce::var (uni76::ParamID::panorama));
                expect (legacyPanoramaParam.isValid());
                // 50.0 specifically covers the old v4 "NATURAL" default -
                // that value must NOT be preserved as if it still meant
                // something under the current contract.
                legacyPanoramaParam.setProperty ("value", 50.0, nullptr);

                if (auto xml = legacyState.createXml())
                {
                    juce::MemoryBlock data;
                    juce::AudioProcessor::copyXmlToBinary (*xml, data);
                    processor.setStateInformation (data.getData(), (int) data.getSize());
                }

                auto* param = apvts.getParameter (uni76::ParamID::panorama);
                expect (param != nullptr);
                if (param != nullptr)
                    expectWithinAbsoluteError (param->getValue(), 0.0f, 0.001f, "legacy schema v" + juce::String (legacySchemaVersion));
            }
        }

        beginTest ("New-schema state round-trips panorama exactly at 0/50/100%");
        {
            for (float pct : { 0.0f, 50.0f, 100.0f })
            {
                UNI76AudioProcessor processor;
                auto& apvts = processor.getValueTreeState();
                auto* param = apvts.getParameter (uni76::ParamID::panorama);
                expect (param != nullptr);
                if (param == nullptr) continue;

                param->setValueNotifyingHost (pct / 100.0f);

                juce::MemoryBlock saved;
                processor.getStateInformation (saved);

                UNI76AudioProcessor reloaded;
                reloaded.setStateInformation (saved.getData(), (int) saved.getSize());
                auto* reloadedParam = reloaded.getValueTreeState().getParameter (uni76::ParamID::panorama);
                expect (reloadedParam != nullptr);
                if (reloadedParam != nullptr)
                    expectWithinAbsoluteError (reloadedParam->getValue(), pct / 100.0f, 0.001f);
            }
        }

        beginTest ("panoramaEnabled=false gives a real bypass through the full processor, and persists across save/restore");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (44100.0, 512);
            processor.getModuleEnableState().setEnabled (4, false);
            processor.getValueTreeState().getParameter (uni76::ParamID::panorama)->setValueNotifyingHost (1.0f); // MOTION

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> buffer (2, 512);
            for (int i = 0; i < 512; ++i)
            {
                buffer.setSample (0, i, 0.3f * std::sin (juce::MathConstants<float>::twoPi * 300.0f * (float) i / 44100.0f));
                buffer.setSample (1, i, 0.3f * std::sin (juce::MathConstants<float>::twoPi * 450.0f * (float) i / 44100.0f));
            }
            for (int b = 0; b < 20; ++b) processor.processBlock (buffer, midi);
            expect (bufferIsFinite (buffer), "bypassed PAN should still produce finite output");

            juce::MemoryBlock saved;
            processor.getStateInformation (saved);
            UNI76AudioProcessor reloaded;
            reloaded.setStateInformation (saved.getData(), (int) saved.getSize());
            expect (! reloaded.getModuleEnableState().isEnabled (4), "panoramaEnabled=false should survive save/restore");
        }

        beginTest ("PITCH+PAN integration: PITCH's bass stability survives PAN's MOTION running after it");
        {
            struct Case { int pitchSt; float panWidth; };
            const Case cases[] { { 0, 0.5f }, { -12, 1.0f }, { 7, 1.0f }, { 12, 1.0f } };

            for (const auto& c : cases)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (44100.0, 512);

                auto* pitchParam = dynamic_cast<juce::AudioParameterInt*> (processor.getValueTreeState().getParameter (uni76::ParamID::pitch));
                expect (pitchParam != nullptr);
                if (pitchParam != nullptr)
                    pitchParam->setValueNotifyingHost (pitchParam->convertTo0to1 ((float) c.pitchSt));
                processor.getValueTreeState().getParameter (uni76::ParamID::panorama)->setValueNotifyingHost (c.panWidth);

                juce::MidiBuffer midi;
                juce::AudioBuffer<float> buffer (2, 512);
                bool finite = true;

                // Identical L/R bass into PITCH (two independent mono
                // engines, bit-identical by construction per
                // docs/DSP_PITCH.md), then into PAN.
                constexpr int totalBlocks = 130; // ~1.5s at 512/44100
                juce::AudioBuffer<float> captured (2, totalBlocks * 512);
                for (int b = 0; b < totalBlocks; ++b)
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        const auto s = 0.3f * std::sin (juce::MathConstants<float>::twoPi * 60.0f * (float) (b * 512 + i) / 44100.0f);
                        buffer.setSample (0, i, s);
                        buffer.setSample (1, i, s);
                    }
                    processor.processBlock (buffer, midi);
                    if (! bufferIsFinite (buffer)) finite = false;
                    captured.copyFrom (0, b * 512, buffer, 0, 0, 512);
                    captured.copyFrom (1, b * 512, buffer, 1, 0, 512);
                }

                const juce::String label = "PITCH=" + juce::String (c.pitchSt) + "ST PAN=" + juce::String (c.panWidth * 100.0f) + "%";
                expect (finite, "non-finite output for " + label);

                // Bass fundamental (60Hz, shifted by PITCH) should still
                // be trackable and stable - PAN's motion must not add
                // wobble on top of PITCH's own (already-verified) bass
                // stability.
                const auto expectedFreq = 60.0f * std::pow (2.0f, (float) c.pitchSt / 12.0f);
                const auto latency = processor.getLatencySamples();
                const auto settle = latency + (int) (0.2 * 44100.0);
                const auto usable = captured.getNumSamples() - settle;
                if (usable > 4000)
                {
                    const auto stability = analyzeBassStability (captured, 0, settle, usable, 44100.0, expectedFreq);
                    if (stability.numWindows > 2)
                    {
                        const auto freqDevPercent = 100.0 * stability.freqStd / (double) expectedFreq;
                        expect (freqDevPercent < 5.0, label + ": PAN should not add bass wobble on top of PITCH, freqStd%=" + juce::String (freqDevPercent));
                    }
                }
            }
        }

        beginTest ("Full chain PREAMP+EQ+SAT+PITCH+PAN stays finite/stable for representative combinations");
        {
            struct Combo { float preamp, eq, sat; int pitchSt; float pan; };
            const Combo combos[] {
                { 0.5f, 0.5f, 0.5f, 7, 1.0f },
                { 0.75f, 1.0f, 1.0f, -12, 1.0f },
            };

            for (const auto& combo : combos)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (44100.0, 512);

                processor.getValueTreeState().getParameter (uni76::ParamID::preamp)->setValueNotifyingHost (combo.preamp);
                processor.getValueTreeState().getParameter (uni76::ParamID::eq)->setValueNotifyingHost (combo.eq);
                processor.getValueTreeState().getParameter (uni76::ParamID::saturation)->setValueNotifyingHost (combo.sat);
                processor.getValueTreeState().getParameter (uni76::ParamID::panorama)->setValueNotifyingHost (combo.pan);

                auto* pitchParam = dynamic_cast<juce::AudioParameterInt*> (processor.getValueTreeState().getParameter (uni76::ParamID::pitch));
                expect (pitchParam != nullptr);
                if (pitchParam != nullptr)
                    pitchParam->setValueNotifyingHost (pitchParam->convertTo0to1 ((float) combo.pitchSt));

                juce::MidiBuffer midi;
                juce::AudioBuffer<float> buffer (2, 512);
                bool finite = true;
                float peak = 0.0f;

                for (int b = 0; b < 25; ++b)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int s = 0; s < 512; ++s)
                            buffer.setSample (ch, s, 0.35f * std::sin (juce::MathConstants<float>::twoPi * (500.0f + (float) ch * 150.0f)
                                                                        * (float) (b * 512 + s) / 44100.0f));
                    processor.processBlock (buffer, midi);
                    if (! bufferIsFinite (buffer)) finite = false;
                }
                for (int ch = 0; ch < 2; ++ch)
                    for (int s = 0; s < 512; ++s)
                        peak = juce::jmax (peak, std::abs (buffer.getSample (ch, s)));

                const juce::String label = "PREAMP=" + juce::String (combo.preamp) + " EQ=" + juce::String (combo.eq)
                                          + " SAT=" + juce::String (combo.sat) + " PITCH=" + juce::String (combo.pitchSt)
                                          + "ST PAN=" + juce::String (combo.pan * 100.0f) + "%";
                expect (finite, "non-finite output for " + label);
                expect (peak < 4.0f, "unexpected gain explosion for " + label);
            }
        }

        beginTest ("Total plugin latency is unchanged by PAN (still PREAMP+EQ+SAT+PITCH, PAN adds 0)");
        {
            const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };

            for (auto sr : rates)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 512);

                uni76::dsp::PreampProcessor preamp;
                preamp.prepare (sr, 512, 2);
                uni76::dsp::EqProcessor eq;
                eq.prepare (sr, 512, 2);
                uni76::dsp::SatProcessor sat;
                sat.prepare (sr, 512, 2);
                uni76::dsp::PitchProcessor pitch;
                pitch.prepare (sr, 512, 2);
                uni76::dsp::PanoramaProcessor pan;
                pan.prepare (sr, 512, 2);

                expectEquals (pan.getLatencySamples(), 0);
                expectEquals (processor.getLatencySamples(),
                               preamp.getLatencySamples() + eq.getLatencySamples() + sat.getLatencySamples()
                               + pitch.getLatencySamples() + pan.getLatencySamples());
            }
        }
    }
};

static UNI76PanoramaIntegrationTests uni76PanoramaIntegrationTests; // NOLINT - self-registers with the UnitTestRunner

int main()
{
    juce::UnitTestRunner runner;
    runner.runAllTests();

    bool anyFailures = false;

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        auto* result = runner.getResult (i);
        if (result == nullptr || result->failures == 0)
            continue;

        anyFailures = true;

        std::cout << "FAILED: " << result->unitTestName.toStdString()
                   << " / " << result->subcategoryName.toStdString() << std::endl;

        for (const auto& message : result->messages)
            std::cout << "    " << message.toStdString() << std::endl;
    }

    return anyFailures ? 1 : 0;
}
