#include <juce_audio_processors/juce_audio_processors.h>

#include "Plugin/PluginProcessor.h"
#include "Parameters/ParameterIDs.h"
#include "Core/PluginIdentity.h"

#include <cstring>

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

        beginTest ("All 7 parameter IDs exist with 50% defaults");
        {
            UNI76AudioProcessor processor;
            auto& apvts = processor.getValueTreeState();

            expectEquals ((int) uni76::ParamID::all.size(), 7);

            for (const auto* id : uni76::ParamID::all)
            {
                auto* param = apvts.getParameter (id);
                expect (param != nullptr, juce::String ("missing parameter: ") + id);

                if (auto* floatParam = dynamic_cast<juce::AudioParameterFloat*> (param))
                    expectWithinAbsoluteError (floatParam->get(), 50.0f, 0.001f, id);
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

                if (auto* param = apvts.getParameter (id))
                    expectWithinAbsoluteError (param->getValue(), 0.5f, 0.001f, id);
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

int main()
{
    juce::UnitTestRunner runner;
    runner.runAllTests();

    for (int i = 0; i < runner.getNumResults(); ++i)
        if (auto* result = runner.getResult (i); result != nullptr && result->failures > 0)
            return 1;

    return 0;
}
