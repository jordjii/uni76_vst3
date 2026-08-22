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
                const auto expectedDefault = std::strcmp (id, uni76::ParamID::eq) == 0 ? 0.5f : 0.0f;

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
                { 192000.0, 512, false },
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

            // At DRIVE=100%, harmonic content must be clearly present but
            // still "musical", not a wall of noise.
            expect (results.back().thdPercent > 0.5f, "DRIVE=100% should produce clearly measurable harmonic content");
            expect (results.back().thdPercent < 60.0f, "DRIVE=100% THD should stay in a musical range, not explode");
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
