#include <juce_audio_processors/juce_audio_processors.h>

#include "Plugin/PluginProcessor.h"
#include "Parameters/ParameterIDs.h"
#include "Core/PluginIdentity.h"
#include "Core/LevelMeter.h"
#include "Core/MeterEnvelope.h"
#include "Core/ModuleEnableState.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

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

        beginTest ("processBlock is a transparent passthrough across sample rates/block sizes, with no latency");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));

            struct Config { double sampleRate; int blockSize; };
            const Config configs[] { { 44100.0, 64 }, { 48000.0, 512 }, { 96000.0, 1 }, { 44100.0, 4096 } };

            juce::Random random (1234);

            for (const auto& config : configs)
            {
                processor.prepareToPlay (config.sampleRate, config.blockSize);

                juce::AudioBuffer<float> buffer (2, config.blockSize);

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    for (int i = 0; i < buffer.getNumSamples(); ++i)
                        buffer.setSample (ch, i, random.nextFloat() * 2.0f - 1.0f);

                juce::AudioBuffer<float> reference;
                reference.makeCopyOf (buffer);

                juce::MidiBuffer midi;
                processor.processBlock (buffer, midi);

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                {
                    const auto matches = std::memcmp (buffer.getReadPointer (ch),
                                                       reference.getReadPointer (ch),
                                                       sizeof (float) * (size_t) buffer.getNumSamples()) == 0;
                    expect (matches, "processBlock modified the audio buffer");
                }

                processor.releaseResources();
            }

            expectEquals (processor.getLatencySamples(), 0);
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

        beginTest ("Processor integration: processBlock() feeds both meters without disturbing passthrough or latency");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (44100.0, 256);

            auto buffer = makeConstantBuffer (2, 256, 0.8f);

            juce::AudioBuffer<float> reference;
            reference.makeCopyOf (buffer);

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                const auto matches = std::memcmp (buffer.getReadPointer (ch),
                                                   reference.getReadPointer (ch),
                                                   sizeof (float) * (size_t) buffer.getNumSamples()) == 0;
                expect (matches, "processBlock modified the audio buffer while feeding the meters");
            }

            expectEquals (processor.getLatencySamples(), 0);

            expectWithinAbsoluteError (processor.getInputLevelMeter().readAndResetPeak(), 0.8f, 0.0001f);
            expectWithinAbsoluteError (processor.getOutputLevelMeter().readAndResetPeak(), 0.8f, 0.0001f);
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
