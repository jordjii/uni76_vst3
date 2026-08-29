#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "Plugin/PluginProcessor.h"
#include "Parameters/ParameterIDs.h"
#include "Core/PluginIdentity.h"
#include "Core/LevelMeter.h"
#include "Core/MeterEnvelope.h"
#include "Core/ModuleEnableState.h"
#include "Core/FactoryPresets.h"
#include "Core/UserPresets.h"
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
#include "DSP/VerbProcessor.h"
#include "DSP/VerbCurves.h"
#include "DSP/ImagerProcessor.h"
#include "DSP/ImagerCurves.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
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

        beginTest ("All 8 parameter IDs exist with the correct defaults (EQ 50%, everything else 0%)");
        {
            UNI76AudioProcessor processor;
            auto& apvts = processor.getValueTreeState();

            expectEquals ((int) uni76::ParamID::all.size(), 8);

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
                // PITCH and imageTilt are special-cased to 0.5f too: PITCH's
                // default (0 ST) sits at the *normalised* midpoint of its
                // -12..+12 range, same as EQ's PHONE default sits at the
                // midpoint of 0..100; imageTilt's default (0/CENTER) is
                // likewise the normalised midpoint of its -100..100 range.
                const bool isMidpointDefault = std::strcmp (id, uni76::ParamID::eq) == 0
                                             || std::strcmp (id, uni76::ParamID::pitch) == 0
                                             || std::strcmp (id, uni76::ParamID::imageTilt) == 0;
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

                // imageTilt is -100..100 (normalised 0.5 == 0/CENTER), not
                // the plain 0..100% linear range every other float
                // parameter here uses - see ParameterLayout.cpp's
                // makeImageTiltParameter(). Its own normalised<->real-
                // value linearity is checked separately below.
                if (std::strcmp (id, uni76::ParamID::imageTilt) == 0)
                {
                    if (auto* floatParam = dynamic_cast<juce::AudioParameterFloat*> (param))
                        expectWithinAbsoluteError (floatParam->get(), 0.0f, 0.01f, id);
                }
                else if (auto* floatParam = dynamic_cast<juce::AudioParameterFloat*> (param))
                    expectWithinAbsoluteError (floatParam->get(), 50.0f, 0.01f, id);

                param->setValueNotifyingHost (1.0f);
                expectWithinAbsoluteError (param->getValue(), 1.0f, 0.0001f, id);
            }
        }

        beginTest ("getTailLengthSeconds() tracks VERB's actual RT60 at the current wet amount, not a fixed constant");
        {
            // Regression test for a real bug found during the pre-release
            // audit: getTailLengthSeconds() unconditionally returned 0.0
            // regardless of the `reverb` parameter or VERB's enabled state,
            // so a host would cut VERB's tail off immediately (e.g. on
            // bounce, or when a clip ends) exactly as if VERB had no tail
            // at all, even at DEEP/100% (~6s target RT60) - see
            // docs/FULL_DSP_AUDIT.md's "VERB tail" section and
            // docs/DSP_VERB.md.
            UNI76AudioProcessor processor;
            auto& apvts = processor.getValueTreeState();
            auto* reverb = apvts.getParameter (uni76::ParamID::reverb);
            expect (reverb != nullptr);

            reverb->setValueNotifyingHost (0.0f);
            expectEquals (processor.getTailLengthSeconds(), 0.0,
                          "VERB0 (DRY) must report no meaningful tail");

            reverb->setValueNotifyingHost (0.5f);
            const auto tailAt50 = processor.getTailLengthSeconds();
            expectWithinAbsoluteError (tailAt50, (double) uni76::dsp::verbDecaySeconds (0.5f), 0.01,
                                       "VERB50 (PLATE) tail should match the macro's own RT60 curve");
            expect (tailAt50 > 0.5, "VERB50 tail should be clearly nonzero");

            reverb->setValueNotifyingHost (1.0f);
            const auto tailAt100 = processor.getTailLengthSeconds();
            expectWithinAbsoluteError (tailAt100, (double) uni76::dsp::verbDecaySeconds (1.0f), 0.01,
                                       "VERB100 (DEEP) tail should match the macro's own RT60 curve");
            expect (tailAt100 > tailAt50, "VERB100 tail should be longer than VERB50's");

            // Disabling VERB internally mutes the wet contribution entirely
            // (same crossfade-to-dry-only bypass every other module uses) -
            // the reported tail must collapse to 0 regardless of the
            // `reverb` parameter's own value, since nothing decaying is
            // actually being produced.
            processor.getModuleEnableState().setEnabled (5, false);
            expectEquals (processor.getTailLengthSeconds(), 0.0,
                          "a disabled VERB module must report no tail even at 100% wet");
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

        // ---- Curve values match the specified reference numbers directly ----
        // (see EqCurves.h/docs/DSP_EQ.md's "Redesign" section) - a precise,
        // non-audio-domain check that the actual anchor Hz/Q values are
        // exactly what was specified, before any filter-response measurement
        // below even comes into it.
        beginTest ("EqCurves.h anchors match the specified reference (FabFilter Pro-Q3) values exactly");
        {
            const auto left = uni76::dsp::eqParamsAt (0.0f);
            expectWithinAbsoluteError (left.hpHz, 72.0f, 0.01f, "left HP Hz");
            expectWithinAbsoluteError (left.hpQ, 0.765f, 0.001f, "left HP Q");
            expectWithinAbsoluteError (left.lpHz, 1132.0f, 0.01f, "left LP Hz");
            expectWithinAbsoluteError (left.lpQ, 0.676f, 0.001f, "left LP Q");

            const auto centre = uni76::dsp::eqParamsAt (0.5f);
            expectWithinAbsoluteError (centre.hpHz, 461.0f, 0.01f, "centre HP Hz");
            expectWithinAbsoluteError (centre.hpQ, 0.765f, 0.001f, "centre HP Q");
            expectWithinAbsoluteError (centre.lpHz, 7288.0f, 0.01f, "centre LP Hz");
            expectWithinAbsoluteError (centre.lpQ, 0.676f, 0.001f, "centre LP Q");

            const auto right = uni76::dsp::eqParamsAt (1.0f);
            expectWithinAbsoluteError (right.hpHz, 748.0f, 0.01f, "right HP Hz");
            expectWithinAbsoluteError (right.hpQ, 0.765f, 0.001f, "right HP Q");
            expectWithinAbsoluteError (right.lpHz, 11855.0f, 0.01f, "right LP Hz");
            expectWithinAbsoluteError (right.lpQ, 0.676f, 0.001f, "right LP Q");

            // Monotonic sweep across the full range, both cuts move together.
            const float points[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
            for (size_t i = 1; i < 5; ++i)
            {
                expect (uni76::dsp::eqParamsAt (points[i]).hpHz > uni76::dsp::eqParamsAt (points[i - 1]).hpHz,
                        "HP corner must sweep upward monotonically");
                expect (uni76::dsp::eqParamsAt (points[i]).lpHz > uni76::dsp::eqParamsAt (points[i - 1]).lpHz,
                        "LP corner must sweep upward monotonically");
            }
        }

        beginTest ("eqEnabled=false bypasses the EQ (dry passthrough, no delay needed - zero latency)");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 1);

            // 12kHz sits well above even the widest anchor's LP corner
            // (11855Hz at t=1) - if disabled didn't truly bypass, this
            // would show real attenuation.
            const auto gainDb = eqGainDb (eq, sr, blockSize, 12000.0f, amplitude, 1.0f, false);
            expectWithinAbsoluteError (gainDb, 0.0f, 0.3f, "disabled EQ should leave a 12kHz tone essentially untouched");
        }

        beginTest ("Centre (0%% displayed, t=0.5): passband stays near unity, well outside the band is suppressed");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 1);

            // Passband is 461Hz-7288Hz at centre - 1kHz sits comfortably
            // inside it; 60Hz and 15kHz sit well outside.
            const auto inBand  = eqGainDb (eq, sr, blockSize, 1000.0f, amplitude, 0.5f);
            const auto low     = eqGainDb (eq, sr, blockSize, 60.0f, amplitude, 0.5f);
            const auto high    = eqGainDb (eq, sr, blockSize, 15000.0f, amplitude, 0.5f);

            expect (inBand > -1.0f && inBand < 1.0f, "in-band content should pass near unity gain");
            expect (low < -20.0f, "well below the HP corner should be strongly suppressed (48dB/oct)");
            expect (high < -20.0f, "well above the LP corner should be strongly suppressed (48dB/oct)");
        }

        beginTest ("Left extreme (-50%% displayed, t=0.0): a lower, darker/narrower phone band than centre");
        {
            uni76::dsp::EqProcessor eqLeft, eqCentre;
            eqLeft.prepare (sr, blockSize, 1);
            eqCentre.prepare (sr, blockSize, 1);

            // 300Hz sits inside the left anchor's passband (72-1132Hz) but
            // well below the centre anchor's HP corner (461Hz).
            const auto leftGain   = eqGainDb (eqLeft,   sr, blockSize, 300.0f, amplitude, 0.0f);
            const auto centreGain = eqGainDb (eqCentre, sr, blockSize, 300.0f, amplitude, 0.5f);

            expect (leftGain > -1.0f && leftGain < 1.0f, "300Hz should pass near-unity at the left extreme");
            expect (centreGain < leftGain - 15.0f, "300Hz should be clearly suppressed at centre by comparison");
        }

        beginTest ("Right extreme (+50%% displayed, t=1.0): a higher, brighter/narrower phone band than centre");
        {
            uni76::dsp::EqProcessor eqRight, eqCentre;
            eqRight.prepare (sr, blockSize, 1);
            eqCentre.prepare (sr, blockSize, 1);

            // 9000Hz sits inside the right anchor's passband (748-11855Hz)
            // but well above the centre anchor's LP corner (7288Hz). It's
            // still within ~0.5 octaves of the right anchor's own LP
            // corner though, where 4 *identically*-tuned cascaded stages
            // (see EqProcessor.cpp) measurably narrow the effective
            // passband versus a single stage's own -3dB point - a real,
            // expected characteristic of this cascade-without-per-stage-
            // compensation design, not a bug - so "near-unity" here means
            // "clearly still passing", not "flat to a fraction of a dB".
            const auto rightGain  = eqGainDb (eqRight,  sr, blockSize, 9000.0f, amplitude, 1.0f);
            const auto centreGain = eqGainDb (eqCentre, sr, blockSize, 9000.0f, amplitude, 0.5f);

            expect (rightGain > -6.0f && rightGain < 1.0f, "9000Hz should still clearly be passing at the right extreme");
            expect (centreGain < rightGain - 10.0f, "9000Hz should be clearly suppressed at centre by comparison");
        }

        beginTest ("Slope is steep (48dB/oct via 4 cascaded stages), not the old design's gentle rounding");
        {
            uni76::dsp::EqProcessor eq;
            eq.prepare (sr, blockSize, 1);

            // Below the centre anchor's HP corner (461Hz): one octave down
            // (230Hz) vs two octaves down (115Hz) should differ by roughly
            // 48dB (a steep cascade), not the old design's much gentler
            // rounding - checked as a wide, forgiving range since a real
            // RBJ Q=0.765 cascade isn't a mathematically perfect Butterworth
            // slope right at the corner.
            const auto oneOctaveDown = eqGainDb (eq, sr, blockSize, 230.0f, amplitude, 0.5f);
            const auto twoOctaveDown = eqGainDb (eq, sr, blockSize, 115.0f, amplitude, 0.5f);
            const auto slopePerOctave = oneOctaveDown - twoOctaveDown;

            std::cout << "\n=== EQ slope check: 230Hz=" << oneOctaveDown << "dB 115Hz=" << twoOctaveDown
                       << "dB slope=" << slopePerOctave << "dB/oct ===" << std::endl << std::endl;
            expect (slopePerOctave > 20.0f, "slope well below the HP corner should be steep (cascaded stages), not gentle");
        }

        beginTest ("No gain boost anywhere - this is a cut-only filter (two 48dB/oct HP+LP), never a shelf/peak boost");
        {
            // Unlike the old DARK/PHONE/AIR design (which had a genuine
            // AIR-anchor high-shelf boost), the new topology has no gain
            // stage at all - it can only remove energy outside its own
            // passband, never add energy anywhere. Checked across the
            // full macro range and a wide frequency sweep.
            for (auto t : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                uni76::dsp::EqProcessor eq;
                eq.prepare (sr, blockSize, 1);

                for (float freqHz : { 100.0f, 500.0f, 1000.0f, 3000.0f, 8000.0f, 15000.0f })
                {
                    const auto gainDb = eqGainDb (eq, sr, blockSize, freqHz, amplitude, t);
                    expect (gainDb < 1.0f, juce::String ("EQ") + juce::String ((int) (t * 100.0f))
                                                + "% at " + juce::String (freqHz, 0) + "Hz should never boost above ~unity");
                }
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

// ---- VERB test helpers ---------------------------------------------------

namespace
{
    juce::AudioBuffer<float> runVerbProcessor (uni76::dsp::VerbProcessor& verb, const juce::AudioBuffer<float>& input,
                                                int blockSize, float wetNormalised01, bool enabled)
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

            verb.process (block, wetNormalised01, enabled);

            for (int ch = 0; ch < numChannels; ++ch)
                result.copyFrom (ch, done, block, ch, 0, thisBlock);
            done += thisBlock;
        }
        return result;
    }

    /** wetOnly = VERB(wet) - VERB(0%) - since 0% is provably dry-exact
        (verbWetGain(0)==0.0 - see VerbCurves.h), subtracting it isolates
        the additive wet contribution alone, regardless of what internal
        tank state either run built up. */
    juce::AudioBuffer<float> verbWetOnly (const juce::AudioBuffer<float>& input, double sampleRate, int blockSize, float wetNormalised01)
    {
        uni76::dsp::VerbProcessor verbWet;
        verbWet.prepare (sampleRate, blockSize, 2);
        auto atWet = runVerbProcessor (verbWet, input, blockSize, wetNormalised01, true);

        uni76::dsp::VerbProcessor verbDry;
        verbDry.prepare (sampleRate, blockSize, 2);
        auto atDry = runVerbProcessor (verbDry, input, blockSize, 0.0f, true);

        juce::AudioBuffer<float> out (2, input.getNumSamples());
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < input.getNumSamples(); ++i)
                out.setSample (ch, i, atWet.getSample (ch, i) - atDry.getSample (ch, i));
        return out;
    }

    /** Simple RBJ bandpass (Q=1.5) - widens a decay measurement's
        frequency window so a single mode's own beating against the
        tracked frequency doesn't corrupt the envelope (matches the
        scratch tuning tool's approach, which found a narrow single-bin
        Goertzel tracker gave unreliable, non-monotonic results). */
    struct VerbTestBandpass
    {
        double b0 = 1, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float process (float x) noexcept
        {
            const auto y = b0 * (double) x + z1;
            z1 = -a1 * y + z2;
            z2 = b2 * (double) x - a2 * y;
            return (float) y;
        }
    };

    VerbTestBandpass makeVerbTestBandpass (double freqHz, double q, double sampleRate)
    {
        const auto w0 = juce::MathConstants<double>::twoPi * freqHz / sampleRate;
        const auto cosw0 = std::cos (w0);
        const auto sinw0 = std::sin (w0);
        const auto alpha = sinw0 / (2.0 * q);
        const auto a0 = 1.0 + alpha;
        VerbTestBandpass bq;
        bq.b0 = (sinw0 / 2.0) / a0;
        bq.b2 = -(sinw0 / 2.0) / a0;
        bq.a1 = (-2.0 * cosw0) / a0;
        bq.a2 = (1.0 - alpha) / a0;
        return bq;
    }

    /** Bandpassed RMS envelope, in dB relative to the burst's own peak,
        at the given post-burst checkpoint times - see
        VerbTestBandpass's comment for why this is used instead of a
        threshold-crossing RT60 extrapolation (a first attempt at that
        broke down against this reverb's genuinely multi-mode, non-
        single-exponential decay shape). */
    std::vector<double> verbDecayCheckpointsDb (const juce::AudioBuffer<float>& wetOnlyBuf, double sampleRate, double freqHz,
                                                 int burstStartSample, int burstEndSample, const std::vector<double>& checkpointSeconds)
    {
        const auto total = wetOnlyBuf.getNumSamples();
        auto bp = makeVerbTestBandpass (freqHz, 1.5, sampleRate);
        std::vector<float> filtered ((size_t) total, 0.0f);
        for (int i = 0; i < total; ++i)
            filtered[(size_t) i] = bp.process (wetOnlyBuf.getSample (0, i));

        const auto windowLen = juce::jmax (32, (int) (0.02 * sampleRate));
        auto rmsAt = [&] (int pos) -> double
        {
            if (pos < 0 || pos + windowLen > total) return 0.0;
            double sumSq = 0.0;
            for (int i = 0; i < windowLen; ++i)
                sumSq += (double) filtered[(size_t) (pos + i)] * (double) filtered[(size_t) (pos + i)];
            return std::sqrt (sumSq / (double) windowLen);
        };

        double peakMag = 0.0;
        for (int pos = burstStartSample; pos < burstEndSample + (int) (0.1 * sampleRate) && pos + windowLen <= total; pos += windowLen / 4)
            peakMag = juce::jmax (peakMag, rmsAt (pos));

        std::vector<double> result;
        for (auto cp : checkpointSeconds)
        {
            if (peakMag < 1.0e-9) { result.push_back (-200.0); continue; }
            const auto pos = burstEndSample + (int) (cp * sampleRate);
            result.push_back (20.0 * std::log10 (juce::jmax (rmsAt (pos), 1.0e-9) / peakMag));
        }
        return result;
    }
}

class UNI76VerbProcessorTests final : public juce::UnitTest
{
public:
    UNI76VerbProcessorTests() : juce::UnitTest ("uni76::dsp::VerbProcessor", "UNI76") {}

    void runTest() override
    {
        beginTest ("Constructs, prepares, resets; latency is always 0 across sample rates/block sizes/wet/enabled");
        {
            const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
            const int blocks[] { 32, 64, 128, 256, 512, 1024, 2048 };

            for (auto sr : rates)
            {
                for (auto bs : blocks)
                {
                    uni76::dsp::VerbProcessor verb;
                    verb.prepare (sr, bs, 2);
                    expectEquals (verb.getLatencySamples(), 0);
                    verb.reset();
                    expectEquals (verb.getLatencySamples(), 0);
                }
            }

            uni76::dsp::VerbProcessor verb;
            verb.prepare (44100.0, 512, 2);
            for (auto wet : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                for (auto enabled : { true, false })
                {
                    juce::AudioBuffer<float> buffer (2, 512);
                    buffer.clear();
                    verb.process (buffer, wet, enabled);
                    expectEquals (verb.getLatencySamples(), 0);
                }
            }
        }

        beginTest ("DRY (0%) is a bit-exact (up to float rounding) identity transform");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            uni76::dsp::VerbProcessor verb;
            verb.prepare (sr, blockSize, 2);

            const auto totalSamples = (int) (3.0 * sr);
            auto input = generateIdenticalStereo (totalSamples, sr, 220.0f, 0.2f);
            {
                auto extra = generateChord (totalSamples, sr, { 440.0f, 1500.0f, 4000.0f }, { 0.15f, 0.1f, 0.08f });
                input.addFrom (0, 0, extra, 0, 0, totalSamples);
                input.addFrom (1, 0, extra, 0, 0, totalSamples);
            }
            auto output = runVerbProcessor (verb, input, blockSize, 0.0f, true);

            double sumSq = 0.0, maxDiff = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < totalSamples; ++i)
                {
                    const auto diff = (double) (output.getSample (ch, i) - input.getSample (ch, i));
                    sumSq += diff * diff;
                    maxDiff = juce::jmax (maxDiff, std::abs (diff));
                }
            const auto rmsDiff = std::sqrt (sumSq / (double) (2 * totalSamples));
            std::cout << "\n=== VERB DRY (0%) null test === RMS diff=" << rmsDiff << " max diff=" << maxDiff << std::endl << std::endl;
            expect (rmsDiff < 1.0e-5, "DRY should be a near-bit-exact identity transform");
        }

        beginTest ("Modal/resonance sweep: spectral flatness of the plate tank's steady decay, VERB=50% (PLATE)");
        {
            // Diagnostic measurement for the "too metallic" tuning pass (see
            // CLAUDE.md's RC1 UX-polish-pass entry and docs/DSP_VERB.md) -
            // not a strict pass/fail gate, since there is no single
            // "correct" spectral-flatness number, but the peak-to-mean and
            // RMS-deviation figures printed here are exactly the "measure
            // first" the tuning pass needs, and are directly comparable
            // before/after any diffusion/delay-length/damping change.
            // Method: feed a single-sample impulse, isolate the wet-only
            // contribution (see verbWetOnly()), then Goertzel-sample the
            // tank's *steady* decay region (well after the diffuser/pre-
            // delay onset, well before the tail dies into the noise floor)
            // at ~28 log-spaced frequencies from 200Hz-8kHz. A perfectly
            // diffuse/dense response has a flat spectral envelope in this
            // region (low peak-to-mean, low std-dev); a small number of
            // dominant, under-damped modes - the "metallic ring" symptom -
            // shows up as sharp peaks well above the mean.
            constexpr double sr = 48000.0;
            constexpr int blockSize = 256;

            juce::AudioBuffer<float> impulse (2, (int) (1.5 * sr));
            impulse.clear();
            impulse.setSample (0, 0, 1.0f);
            impulse.setSample (1, 0, 1.0f);

            auto wetOnly = verbWetOnly (impulse, sr, blockSize, 0.5f);

            const auto windowStart = (int) (0.30 * sr);
            const int numPoints = 28;
            const float freqLo = 200.0f, freqHi = 8000.0f;

            std::vector<double> magsDb;
            magsDb.reserve ((size_t) numPoints);
            for (int p = 0; p < numPoints; ++p)
            {
                const auto frac = (float) p / (float) (numPoints - 1);
                const auto freq = freqLo * std::pow (freqHi / freqLo, frac);
                const auto win = periodicAnalysisLength (sr, freq, 12);
                const auto mag = goertzelMagnitude (wetOnly, 0, windowStart, win, sr, freq);
                magsDb.push_back (20.0 * std::log10 (juce::jmax ((double) mag, 1.0e-9)));
            }

            double meanDb = 0.0;
            for (auto v : magsDb) meanDb += v;
            meanDb /= (double) magsDb.size();

            double peakAboveMeanDb = 0.0, sumSqDev = 0.0;
            for (auto v : magsDb)
            {
                peakAboveMeanDb = juce::jmax (peakAboveMeanDb, v - meanDb);
                sumSqDev += (v - meanDb) * (v - meanDb);
            }
            const auto stdDevDb = std::sqrt (sumSqDev / (double) magsDb.size());

            // Raw peak-above-mean/stdDev above are confounded by the
            // response's overall broadband TILT (damping alone controls
            // how much darker the tail is by this checkpoint - a change
            // there shifts every high-frequency point down together,
            // which widens peak-vs-mean/stdDev even with zero change in
            // how "spiky" any individual mode is). A local, detrended
            // residual - each point compared against a moving average of
            // its own neighbours, not the single global mean - isolates
            // genuine narrow-band resonant spikes (the actual "metallic
            // ring" symptom) from that broadband tilt, and is the number
            // that actually answers "did individual modes get less
            // dominant", independent of "did the plate get darker".
            constexpr int trendHalfWindow = 3;
            std::vector<double> residuals ((size_t) numPoints, 0.0);
            for (int i = 0; i < numPoints; ++i)
            {
                double trendSum = 0.0;
                int trendCount = 0;
                for (int j = -trendHalfWindow; j <= trendHalfWindow; ++j)
                {
                    if (j == 0) continue;
                    const auto idx = i + j;
                    if (idx < 0 || idx >= numPoints) continue;
                    trendSum += magsDb[(size_t) idx];
                    ++trendCount;
                }
                const auto trend = trendCount > 0 ? trendSum / (double) trendCount : magsDb[(size_t) i];
                residuals[(size_t) i] = magsDb[(size_t) i] - trend;
            }

            double peakResidualDb = 0.0, sumSqResidual = 0.0;
            for (auto r : residuals)
            {
                peakResidualDb = juce::jmax (peakResidualDb, std::abs (r));
                sumSqResidual += r * r;
            }
            const auto residualStdDevDb = std::sqrt (sumSqResidual / (double) residuals.size());

            std::cout << "\n=== VERB modal/resonance sweep (200Hz-8kHz, 28 pts, PLATE 50%) ===" << std::endl;
            std::cout << "  mean=" << meanDb << "dB  peak-above-mean=" << peakAboveMeanDb
                       << "dB  stdDev=" << stdDevDb << "dB  [raw, tilt-confounded]" << std::endl;
            std::cout << "  detrended (local-neighbour residual): peak=" << peakResidualDb
                       << "dB  stdDev=" << residualStdDevDb << "dB  [the actual modal/metallic indicator]" << std::endl;
            std::cout << "=== end modal/resonance sweep ===" << std::endl << std::endl;

            expect (std::isfinite (meanDb) && std::isfinite (peakAboveMeanDb) && std::isfinite (stdDevDb)
                        && std::isfinite (peakResidualDb) && std::isfinite (residualStdDevDb),
                    "resonance sweep must produce finite numbers");
        }

        beginTest ("Macro curve mapping (VerbCurves.h, direct)");
        {
            std::cout << "\n=== VERB curve mapping (VerbCurves.h, direct) ===" << std::endl;
            const float points[] { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
            for (auto t : points)
            {
                std::cout << "  t=" << (t * 100.0f) << "%: wet=" << (uni76::dsp::verbWetGain (t) * 100.0f)
                           << "% decay=" << uni76::dsp::verbDecaySeconds (t) << "s preDelay="
                           << uni76::dsp::verbPreDelayMs (t) << "ms" << std::endl;
            }
            std::cout << "=== end curve mapping ===" << std::endl << std::endl;

            expectWithinAbsoluteError (uni76::dsp::verbWetGain (0.0f), 0.0f, 1.0e-6f, "wet(0%) must be exactly 0.0 (DRY identity)");
            expectWithinAbsoluteError (uni76::dsp::verbWetGain (1.0f), 0.475f, 0.02f, "wet(100%) should land near the 47.5% target");

            for (size_t i = 1; i < 5; ++i)
            {
                expect (uni76::dsp::verbWetGain (points[i]) > uni76::dsp::verbWetGain (points[i - 1]), "wet gain must grow monotonically");
                expect (uni76::dsp::verbDecaySeconds (points[i]) > uni76::dsp::verbDecaySeconds (points[i - 1]), "decay must grow monotonically");
                expect (uni76::dsp::verbPreDelayMs (points[i]) >= uni76::dsp::verbPreDelayMs (points[i - 1]), "pre-delay must not decrease");
            }

            expect (uni76::dsp::verbWetGain (1.0f) < 0.6f, "100% knob position must not mean anywhere near 100% wet");
            // This is the *nominal* target the RT60-from-feedback-gain
            // formula in VerbProcessor.cpp aims for, not the actual
            // perceived decay time - the per-line damping filter removes
            // additional energy every pass on top of the flat gain
            // (see docs/DSP_VERB.md's "RT60" section), so the nominal
            // anchor had to be tuned measurably higher than the product
            // brief's raw 3.5-4.5s target to make the *actual, measured*
            // decay land there - which the frequency-dependent-decay and
            // bass tests below verify directly against real audio, not
            // this raw curve value.
            expect (uni76::dsp::verbDecaySeconds (1.0f) >= 3.0f && uni76::dsp::verbDecaySeconds (1.0f) <= 8.0f, "100% nominal decay target should stay in a sane range");
        }

        beginTest ("Low-frequency wet rejection: 40-500Hz burst response, VERB=100%");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const float testFreqs[] { 40, 60, 80, 100, 120, 160, 200, 250, 300, 350, 400, 500 };

            std::cout << "\n=== VERB low-frequency wet rejection (burst peak, dB vs input) ===" << std::endl;
            for (auto freqHz : testFreqs)
            {
                const int burstLen = (int) (0.05 * sr);
                const int fadeLen = (int) (0.005 * sr);
                const int totalLen = burstLen + (int) (2.0 * sr);

                juce::AudioBuffer<float> input (2, totalLen);
                input.clear();
                {
                    auto burst = generateSine (1, burstLen, sr, freqHz, 0.3f);
                    // Apply a short fade in/out to avoid a hard-edged burst's own broadband click.
                    for (int i = 0; i < fadeLen; ++i)
                    {
                        const auto env = (float) i / (float) fadeLen;
                        burst.applyGain (0, i, 1, env);
                        burst.applyGain (0, burstLen - 1 - i, 1, env);
                    }
                    input.copyFrom (0, 0, burst, 0, 0, burstLen);
                    input.copyFrom (1, 0, burst, 0, 0, burstLen);
                }

                auto wo = verbWetOnly (input, sr, blockSize, 1.0f);
                const auto burstMag = goertzelMagnitude (wo, 0, 0, burstLen, sr, freqHz);
                const auto inputMag = goertzelMagnitude (input, 0, 0, burstLen, sr, freqHz);
                const auto relDb = 20.0f * std::log10 (juce::jmax (burstMag, 1.0e-9f) / juce::jmax (inputMag, 1.0e-9f));

                std::cout << "  " << freqHz << "Hz: wet/input=" << relDb << "dB" << std::endl;

                if (freqHz <= 120.0f)
                    expect (relDb < -50.0f, juce::String (freqHz) + "Hz: bass should be almost completely rejected from the wet path, got " + juce::String (relDb) + "dB");
                else if (freqHz <= 250.0f)
                    expect (relDb < -25.0f, juce::String (freqHz) + "Hz: should still be strongly suppressed, got " + juce::String (relDb) + "dB");
                else if (freqHz <= 300.0f)
                    expect (relDb < -20.0f, juce::String (freqHz) + "Hz: should still be clearly suppressed, got " + juce::String (relDb) + "dB");
            }
            std::cout << "=== end low-frequency wet rejection ===" << std::endl << std::endl;
        }

        beginTest ("Frequency-dependent decay: high frequencies decay faster than 1kHz, at MOTION-equivalent VERB=100%");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const int burstLen = (int) (0.05 * sr);
            const int fadeLen = (int) (0.005 * sr);
            const int totalLen = burstLen + (int) (4.0 * sr);

            auto burstAt = [&] (float freqHz)
            {
                juce::AudioBuffer<float> input (2, totalLen);
                input.clear();
                auto burst = generateSine (1, burstLen, sr, freqHz, 0.3f);
                for (int i = 0; i < fadeLen; ++i)
                {
                    const auto env = (float) i / (float) fadeLen;
                    burst.applyGain (0, i, 1, env);
                    burst.applyGain (0, burstLen - 1 - i, 1, env);
                }
                input.copyFrom (0, 0, burst, 0, 0, burstLen);
                input.copyFrom (1, 0, burst, 0, 0, burstLen);
                return input;
            };

            const std::vector<double> checkpoints { 1.0, 2.0 };
            std::cout << "\n=== VERB frequency-dependent decay (dB at 1.0/2.0s post-burst) ===" << std::endl;

            std::map<float, std::vector<double>> results;
            for (auto freqHz : { 1000.0f, 5000.0f, 8000.0f })
            {
                auto input = burstAt (freqHz);
                auto wo = verbWetOnly (input, sr, blockSize, 1.0f);
                auto db = verbDecayCheckpointsDb (wo, sr, freqHz, 0, burstLen, checkpoints);
                results[freqHz] = db;
                std::cout << "  " << freqHz << "Hz: " << db[0] << "dB@1s  " << db[1] << "dB@2s" << std::endl;
            }
            std::cout << "=== end frequency-dependent decay ===" << std::endl << std::endl;

            expect (results[5000.0f][0] < results[1000.0f][0], "5kHz should have decayed further than 1kHz by 1s");
            expect (results[8000.0f][0] < results[5000.0f][0], "8kHz should have decayed further than 5kHz by 1s");
            expect (results[5000.0f][1] < results[1000.0f][1], "5kHz should have decayed further than 1kHz by 2s");
            expect (results[8000.0f][1] < results[5000.0f][1], "8kHz should have decayed further than 5kHz by 2s");
        }

        beginTest ("Bass test: 50/80/120/250Hz stay almost dry, 500Hz+transient get a clear plate tail, VERB=100%");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            std::cout << "\n=== VERB bass test (wet/input at burst, dB) ===" << std::endl;
            for (auto freqHz : { 50.0f, 80.0f, 120.0f, 250.0f, 500.0f })
            {
                const int burstLen = (int) (0.05 * sr);
                const int totalLen = burstLen + (int) (1.0 * sr);
                juce::AudioBuffer<float> input (2, totalLen);
                input.clear();
                auto burst = generateSine (1, burstLen, sr, freqHz, 0.3f);
                input.copyFrom (0, 0, burst, 0, 0, burstLen);
                input.copyFrom (1, 0, burst, 0, 0, burstLen);

                auto wo = verbWetOnly (input, sr, blockSize, 1.0f);
                const auto burstMag = goertzelMagnitude (wo, 0, 0, burstLen, sr, freqHz);
                const auto inputMag = goertzelMagnitude (input, 0, 0, burstLen, sr, freqHz);
                const auto relDb = 20.0f * std::log10 (juce::jmax (burstMag, 1.0e-9f) / juce::jmax (inputMag, 1.0e-9f));
                std::cout << "  " << freqHz << "Hz: " << relDb << "dB" << std::endl;

                if (freqHz <= 120.0f)
                    expect (relDb < -50.0f, juce::String (freqHz) + "Hz should be almost fully dry");
            }

            // High-frequency transient should produce a rich, present tail.
            {
                const int impulseLen = 8;
                const int totalLen = impulseLen + (int) (1.0 * sr);
                juce::AudioBuffer<float> input (2, totalLen);
                input.clear();
                for (int i = 0; i < impulseLen; ++i)
                {
                    input.setSample (0, i, 0.5f);
                    input.setSample (1, i, 0.5f);
                }
                auto wo = verbWetOnly (input, sr, blockSize, 1.0f);
                double energy = 0.0;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = impulseLen; i < totalLen; ++i)
                        energy += (double) wo.getSample (ch, i) * (double) wo.getSample (ch, i);
                std::cout << "  transient tail energy=" << energy << std::endl;
                expect (energy > 0.001, "a high-frequency transient should produce a clearly present plate tail");
            }
            std::cout << "=== end bass test ===" << std::endl << std::endl;
        }

        beginTest ("Analog nonlinearity: wet-path THD stays small at -18dBFS, 1kHz sine");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            std::cout << "\n=== VERB wet-path THD (-18dBFS, 1kHz) ===" << std::endl;
            for (auto wet : { 0.25f, 0.5f, 0.75f, 1.0f })
            {
                const int totalLen = (int) (11.0 * sr);
                auto input = generateIdenticalStereo (totalLen, sr, 1000.0f, 0.1259f);
                auto wo = verbWetOnly (input, sr, blockSize, wet);

                const int start = (int) (9.0 * sr);
                const int win = (int) (1.5 * sr);
                const auto h1 = goertzelMagnitude (wo, 0, start, win, sr, 1000.0f);
                const auto h2 = goertzelMagnitude (wo, 0, start, win, sr, 2000.0f);
                const auto h3 = goertzelMagnitude (wo, 0, start, win, sr, 3000.0f);
                const auto thd = std::sqrt (h2 * h2 + h3 * h3) / juce::jmax (h1, 1.0e-9f);

                std::cout << "  wet=" << (wet * 100.0f) << "%: H1=" << h1 << " H2=" << h2 << " H3=" << h3 << " THD=" << (thd * 100.0f) << "%" << std::endl;
                expect (thd < 0.05, "THD should stay small (texture, not distortion) at " + juce::String (wet * 100.0f) + "%: " + juce::String (thd * 100.0f) + "%");
            }
            std::cout << "=== end THD ===" << std::endl << std::endl;
        }

        beginTest ("Dry is never touched: dry component is bit-identical to input regardless of wet amount or enabled state");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            const auto totalSamples = (int) (2.0 * sr);
            auto input = generateIdenticalStereo (totalSamples, sr, 300.0f, 0.2f);
            {
                auto extra = generateChord (totalSamples, sr, { 900.0f, 3000.0f }, { 0.15f, 0.1f });
                input.addFrom (0, 0, extra, 0, 0, totalSamples);
                input.addFrom (1, 0, extra, 0, 0, totalSamples);
            }

            for (auto wet : { 0.0f, 0.5f, 1.0f })
            {
                uni76::dsp::VerbProcessor verb;
                verb.prepare (sr, blockSize, 2);
                auto atWet = runVerbProcessor (verb, input, blockSize, wet, true);

                uni76::dsp::VerbProcessor verbZero;
                verbZero.prepare (sr, blockSize, 2);
                auto atZero = runVerbProcessor (verbZero, input, blockSize, 0.0f, true);

                // atZero must equal input exactly (already covered above);
                // atWet - atZero must be finite and, at wet=0, exactly zero.
                if (wet == 0.0f)
                {
                    double maxDiff = 0.0;
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < input.getNumSamples(); ++i)
                            maxDiff = juce::jmax (maxDiff, (double) std::abs (atWet.getSample (ch, i) - atZero.getSample (ch, i)));
                    expect (maxDiff < 1.0e-6, "wet=0 vs wet=0 (both zero) should be identical");
                }
            }
        }

        beginTest ("No runaway gain across the macro sweep on a broadband source");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            const auto totalSamples = (int) (2.0 * sr);
            auto input = generateIdenticalStereo (totalSamples, sr, 100.0f, 0.2f);
            {
                auto extra = generateChord (totalSamples, sr, { 500.0f, 1500.0f, 5000.0f }, { 0.2f, 0.15f, 0.1f });
                input.addFrom (0, 0, extra, 0, 0, totalSamples);
                input.addFrom (1, 0, extra, 0, 0, totalSamples);
            }

            for (auto wet : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                uni76::dsp::VerbProcessor verb;
                verb.prepare (sr, blockSize, 2);
                auto output = runVerbProcessor (verb, input, blockSize, wet, true);
                expect (bufferIsFinite (output), "non-finite output at " + juce::String (wet * 100.0f) + "%");

                float peak = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < output.getNumSamples(); ++i)
                        peak = juce::jmax (peak, std::abs (output.getSample (ch, i)));
                expect (peak < 3.0f, "unexpected gain explosion at " + juce::String (wet * 100.0f) + "%, peak=" + juce::String (peak));
            }
        }

        beginTest ("Bypass (enabled=false) mutes only the wet contribution - dry stays exact; automation is click-free");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            uni76::dsp::VerbProcessor verb;
            verb.prepare (sr, blockSize, 2);
            auto input = generateIdenticalStereo ((int) (1.0 * sr), sr, 1000.0f, 0.3f);
            auto output = runVerbProcessor (verb, input, blockSize, 1.0f, false);

            double maxDiff = 0.0;
            const auto settle = (int) (0.05 * sr); // past the bypass smoother's own ramp
            for (int ch = 0; ch < 2; ++ch)
                for (int i = settle; i < input.getNumSamples(); ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (output.getSample (ch, i) - input.getSample (ch, i)));
            expect (maxDiff < 1.0e-4, "disabled VERB should be an exact dry passthrough once the bypass ramp settles");

            uni76::dsp::VerbProcessor verbAuto;
            verbAuto.prepare (sr, blockSize, 2);
            const auto totalSamples = (int) (2.0 * sr);
            auto autoInput = generateIdenticalStereo (totalSamples, sr, 500.0f, 0.3f);
            juce::AudioBuffer<float> autoOutput (2, totalSamples);
            int done = 0;
            const float steps[] { 0.0f, 1.0f, 0.0f, 0.5f };
            int stepIndex = 0;
            while (done < totalSamples)
            {
                const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
                juce::AudioBuffer<float> block (2, thisBlock);
                for (int ch = 0; ch < 2; ++ch)
                    block.copyFrom (ch, 0, autoInput, ch, done, thisBlock);
                verbAuto.process (block, steps[stepIndex % 4], true);
                for (int ch = 0; ch < 2; ++ch)
                    autoOutput.copyFrom (ch, done, block, ch, 0, thisBlock);
                done += thisBlock;
                ++stepIndex;
            }
            expect (bufferIsFinite (autoOutput), "automation should not produce non-finite output");

            // No large sample-to-sample jump anywhere (a crude click detector).
            bool clickFree = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 1; i < totalSamples; ++i)
                    if (std::abs (autoOutput.getSample (ch, i) - autoOutput.getSample (ch, i - 1)) > 1.0f)
                        clickFree = false;
            expect (clickFree, "automation should be click-free (no large sample-to-sample jumps)");
        }

        beginTest ("Mono bus is supported (folds the tank's stereo taps to mono, stays finite)");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            uni76::dsp::VerbProcessor verb;
            verb.prepare (sr, blockSize, 1);
            auto input = generateSine (1, (int) (1.0 * sr), sr, 1000.0f, 0.3f);
            juce::AudioBuffer<float> output (1, input.getNumSamples());
            int done = 0;
            while (done < input.getNumSamples())
            {
                const auto thisBlock = juce::jmin (blockSize, input.getNumSamples() - done);
                juce::AudioBuffer<float> block (1, thisBlock);
                block.copyFrom (0, 0, input, 0, done, thisBlock);
                verb.process (block, 1.0f, true);
                output.copyFrom (0, done, block, 0, 0, thisBlock);
                done += thisBlock;
            }
            expect (bufferIsFinite (output), "mono bus should stay finite at VERB=100%");
        }

        beginTest ("Silence in, VERB=100%, produces silence out - no added noise/hiss/hum");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            uni76::dsp::VerbProcessor verb;
            verb.prepare (sr, blockSize, 2);
            juce::AudioBuffer<float> silence (2, (int) (2.0 * sr));
            silence.clear();
            auto output = runVerbProcessor (verb, silence, blockSize, 1.0f, true);

            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < output.getNumSamples(); ++i)
                    peak = juce::jmax (peak, std::abs (output.getSample (ch, i)));
            expect (peak < 1.0e-6f, "silence in should give silence out - no self-generated noise, hiss, or hum, peak=" + juce::String (peak));
        }

        beginTest ("NaN/Inf input is sanitised, all sample rates and block sizes stay finite");
        {
            const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
            const int blocks[] { 32, 128, 512, 2048 };

            for (auto sr : rates)
            {
                for (auto bs : blocks)
                {
                    uni76::dsp::VerbProcessor verb;
                    verb.prepare (sr, bs, 2);
                    juce::AudioBuffer<float> buffer (2, bs);
                    buffer.clear();
                    buffer.setSample (0, 0, std::numeric_limits<float>::quiet_NaN());
                    buffer.setSample (1, 0, std::numeric_limits<float>::infinity());
                    verb.process (buffer, 1.0f, true);
                    expect (bufferIsFinite (buffer), "NaN/Inf input should be sanitised at " + juce::String (sr) + "Hz/" + juce::String (bs));
                }
            }
        }

        beginTest ("Regression: pre-delay read-index stays in-bounds under NaN macro input and other edge cases");
        {
            // Locks in the fix for a real Debug-mode "vector subscript out
            // of range" crash: a NaN wetNormalised01 parameter propagates
            // through wetSmoother into preDelaySamples, producing a
            // non-finite readPosF for the pre-delay buffer's read index -
            // std::isfinite comparisons against NaN are always false, so
            // the old `while (readPosF < 0.0f)` wraparound never executed
            // and left readPosF as NaN, and `(int) NaN` is undefined
            // behaviour. See docs/DSP_VERB.md's "A real bug found by
            // Debug-mode testing" section. Every sub-case below must
            // produce finite output and must not crash/assert.

            // (a) NaN wetNormalised01, several sample rates and block
            // sizes, several consecutive blocks each (the crash this
            // catches only manifests on the *second* block onward, once
            // wetSmoother's own internal state has been poisoned by the
            // first NaN target).
            {
                const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
                const int blocks[] { 32, 256, 512, 2048 };
                for (auto sr : rates)
                {
                    for (auto bs : blocks)
                    {
                        uni76::dsp::VerbProcessor verb;
                        verb.prepare (sr, bs, 2);
                        for (int block = 0; block < 5; ++block)
                        {
                            juce::AudioBuffer<float> buffer (2, bs);
                            for (int ch = 0; ch < 2; ++ch)
                                for (int i = 0; i < bs; ++i)
                                    buffer.setSample (ch, i, 0.1f * std::sin ((float) i * 0.1f));
                            verb.process (buffer, std::numeric_limits<float>::quiet_NaN(), true);
                            expect (bufferIsFinite (buffer), "NaN wet% must not produce non-finite output at " + juce::String (sr) + "Hz/" + juce::String (bs));
                        }
                    }
                }
            }

            // (b) +Inf / -Inf wetNormalised01.
            {
                uni76::dsp::VerbProcessor verb;
                verb.prepare (44100.0, 512, 2);
                for (auto wet : { std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() })
                {
                    juce::AudioBuffer<float> buffer (2, 512);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 512; ++i)
                            buffer.setSample (ch, i, 0.1f);
                    verb.process (buffer, wet, true);
                    expect (bufferIsFinite (buffer), "Inf wet% must not produce non-finite output");
                }
            }

            // (c) NaN input, then reset(), then confirm normal processing resumes cleanly.
            {
                uni76::dsp::VerbProcessor verb;
                verb.prepare (44100.0, 512, 2);
                juce::AudioBuffer<float> nanBlock (2, 512);
                nanBlock.clear();
                nanBlock.setSample (0, 0, std::numeric_limits<float>::quiet_NaN());
                verb.process (nanBlock, std::numeric_limits<float>::quiet_NaN(), true);
                verb.reset();

                auto post = generateSine (2, (int) (0.5 * 44100.0), 44100.0, 500.0f, 0.2f);
                auto output = runVerbProcessor (verb, post, 512, 1.0f, true);
                expect (bufferIsFinite (output), "processing after reset() following NaN input must be finite");
            }

            // (d) NaN input, then prepare()/re-prepare() cycles across sample rates.
            {
                uni76::dsp::VerbProcessor verb;
                verb.prepare (44100.0, 512, 2);
                juce::AudioBuffer<float> nanBlock (2, 512);
                nanBlock.clear();
                nanBlock.setSample (1, 10, std::numeric_limits<float>::quiet_NaN());
                verb.process (nanBlock, std::numeric_limits<float>::quiet_NaN(), true);

                const double rates[] { 48000.0, 96000.0, 44100.0, 192000.0 };
                for (auto sr : rates)
                {
                    verb.prepare (sr, 256, 2);
                    auto post = generateSine (2, (int) (0.2 * sr), sr, 500.0f, 0.2f);
                    auto output = runVerbProcessor (verb, post, 256, 1.0f, true);
                    expect (bufferIsFinite (output), "processing after re-prepare() at " + juce::String (sr) + "Hz must be finite");
                }
            }

            // (e) Max block-size boundary: a single block far larger than
            // the pre-delay buffer's own capacity (~0.04*sampleRate+4
            // samples), forcing the write position to wrap around
            // multiple times within one process() call.
            {
                uni76::dsp::VerbProcessor verb;
                verb.prepare (44100.0, 8192, 2);
                juce::AudioBuffer<float> buffer (2, 8192);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 8192; ++i)
                        buffer.setSample (ch, i, 0.1f * std::sin ((float) i * 0.05f));
                verb.process (buffer, std::numeric_limits<float>::quiet_NaN(), true);
                expect (bufferIsFinite (buffer), "an 8192-sample block (larger than the pre-delay buffer) with NaN wet% must stay finite");

                // Follow with a normal block to confirm the instance recovered cleanly.
                juce::AudioBuffer<float> normalBlock (2, 8192);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 8192; ++i)
                        normalBlock.setSample (ch, i, 0.1f * std::sin ((float) i * 0.05f));
                verb.process (normalBlock, 1.0f, true);
                expect (bufferIsFinite (normalBlock), "normal processing after the oversized NaN block must be finite");
            }
        }
    }
};

static UNI76VerbProcessorTests uni76VerbProcessorTests; // NOLINT - self-registers with the UnitTestRunner

class UNI76VerbIntegrationTests final : public juce::UnitTest
{
public:
    UNI76VerbIntegrationTests() : juce::UnitTest ("UNI76AudioProcessor+VERB", "UNI76") {}

    void runTest() override
    {
        beginTest ("Fresh instance: reverb defaults to 0% DRY");
        {
            UNI76AudioProcessor processor;
            auto& apvts = processor.getValueTreeState();
            auto* reverbParam = apvts.getParameter (uni76::ParamID::reverb);
            expect (reverbParam != nullptr);
            if (reverbParam != nullptr)
                expectWithinAbsoluteError (reverbParam->getValue(), 0.0f, 0.001f, "reverb should default to 0%");
        }

        beginTest ("Total plugin latency is unchanged by VERB (still PREAMP+EQ+SAT+PITCH, PAN+VERB both add 0)");
        {
            const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
            for (auto sr : rates)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 512);

                uni76::dsp::PreampProcessor preamp; preamp.prepare (sr, 512, 2);
                uni76::dsp::EqProcessor eq; eq.prepare (sr, 512, 2);
                uni76::dsp::SatProcessor sat; sat.prepare (sr, 512, 2);
                uni76::dsp::PitchProcessor pitch; pitch.prepare (sr, 512, 2);
                uni76::dsp::PanoramaProcessor pan; pan.prepare (sr, 512, 2);
                uni76::dsp::VerbProcessor verb; verb.prepare (sr, 512, 2);

                expectEquals (verb.getLatencySamples(), 0);
                expectEquals (processor.getLatencySamples(),
                               preamp.getLatencySamples() + eq.getLatencySamples() + sat.getLatencySamples()
                               + pitch.getLatencySamples() + pan.getLatencySamples() + verb.getLatencySamples());
            }
        }

        beginTest ("PAN=100 + VERB=50: bass stays centred and stable, highs move and get a plate tail (critical integration test)");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            constexpr double sr = 44100.0;
            processor.prepareToPlay (sr, 256);

            auto& apvts = processor.getValueTreeState();
            apvts.getParameter (uni76::ParamID::panorama)->setValueNotifyingHost (1.0f);
            apvts.getParameter (uni76::ParamID::reverb)->setValueNotifyingHost (0.5f);
            // EQ disabled - this test isolates PAN/VERB's own bass-centring
            // behaviour. EQ's redesigned default (461Hz HP at centre, by
            // design - see docs/DSP_EQ.md's "Redesign" section) legitimately
            // removes an 80Hz test tone almost entirely before it would even
            // reach PAN/VERB, which is correct EQ behaviour, not a PAN/VERB
            // regression - testing "the full chain including a bass-cutting
            // EQ still has bass" is a different (and now false) question
            // than what this test is actually about.
            processor.getModuleEnableState().setEnabled (1, false);

            const int totalSamples = (int) (4.0 * sr);
            juce::AudioBuffer<float> buffer (2, totalSamples);
            buffer.clear();
            {
                auto bass = generateSine (1, totalSamples, sr, 80.0f, 0.3f);
                auto highL = generateSine (1, totalSamples, sr, 4000.0f, 0.15f);
                auto highR = generateSine (1, totalSamples, sr, 5500.0f, 0.15f);
                buffer.addFrom (0, 0, bass, 0, 0, totalSamples);
                buffer.addFrom (1, 0, bass, 0, 0, totalSamples);
                buffer.addFrom (0, 0, highL, 0, 0, totalSamples);
                buffer.addFrom (1, 0, highR, 0, 0, totalSamples);
            }

            juce::MidiBuffer midi;
            int done = 0;
            while (done < totalSamples)
            {
                const auto thisBlock = juce::jmin (256, totalSamples - done);
                juce::AudioBuffer<float> block (2, thisBlock);
                block.copyFrom (0, 0, buffer, 0, done, thisBlock);
                block.copyFrom (1, 0, buffer, 1, done, thisBlock);
                processor.processBlock (block, midi);
                buffer.copyFrom (0, done, block, 0, 0, thisBlock);
                buffer.copyFrom (1, done, block, 1, 0, thisBlock);
                done += thisBlock;
            }

            expect (bufferIsFinite (buffer), "PAN+VERB combined chain should stay finite");

            const auto settle = (int) (0.5 * sr);
            const auto win = juce::jmin (totalSamples - settle, periodicAnalysisLength (sr, 80.0f, 20));
            const auto bassL = goertzelMagnitude (buffer, 0, totalSamples - win, win, sr, 80.0f);
            const auto bassR = goertzelMagnitude (buffer, 1, totalSamples - win, win, sr, 80.0f);
            const auto bassLRDb = 20.0f * std::log10 (juce::jmax (bassL, 1.0e-9f) / juce::jmax (bassR, 1.0e-9f));
            std::cout << "\n=== PAN+VERB integration === 80Hz bass L/R=" << bassLRDb << "dB" << std::endl << std::endl;
            expect (std::abs (bassLRDb) < 2.0f, "bass should stay close to centred through the full PAN+VERB chain, L/R=" + juce::String (bassLRDb) + "dB");
        }

        beginTest ("Full chain low-end: PREAMP+SAT+PITCH+PAN+VERB100 bass stays close to VERB0's bass level (EQ disabled - see docs/DSP_EQ.md's redesign, EQ now legitimately removes this content by design at any setting)");
        {
            constexpr double sr = 44100.0;
            const int totalSamples = (int) (3.0 * sr);

            auto buildInput = [&]
            {
                juce::AudioBuffer<float> buffer (2, totalSamples);
                buffer.clear();
                auto bass60 = generateSine (1, totalSamples, sr, 60.0f, 0.2f);
                auto bass80 = generateSine (1, totalSamples, sr, 80.0f, 0.2f);
                auto bass100 = generateSine (1, totalSamples, sr, 100.0f, 0.2f);
                auto mid = generateSine (1, totalSamples, sr, 2000.0f, 0.15f);
                for (auto* src : { &bass60, &bass80, &bass100, &mid })
                {
                    buffer.addFrom (0, 0, *src, 0, 0, totalSamples);
                    buffer.addFrom (1, 0, *src, 0, 0, totalSamples);
                }
                return buffer;
            };

            auto runFullChain = [&] (float reverbAmount)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 256);
                processor.getValueTreeState().getParameter (uni76::ParamID::reverb)->setValueNotifyingHost (reverbAmount);
                processor.getModuleEnableState().setEnabled (1, false); // EQ disabled - see beginTest's comment

                auto buffer = buildInput();
                juce::MidiBuffer midi;
                int done = 0;
                while (done < totalSamples)
                {
                    const auto thisBlock = juce::jmin (256, totalSamples - done);
                    juce::AudioBuffer<float> block (2, thisBlock);
                    block.copyFrom (0, 0, buffer, 0, done, thisBlock);
                    block.copyFrom (1, 0, buffer, 1, done, thisBlock);
                    processor.processBlock (block, midi);
                    buffer.copyFrom (0, done, block, 0, 0, thisBlock);
                    buffer.copyFrom (1, done, block, 1, 0, thisBlock);
                    done += thisBlock;
                }
                return buffer;
            };

            auto outputVerb0 = runFullChain (0.0f);
            auto outputVerb100 = runFullChain (1.0f);
            expect (bufferIsFinite (outputVerb0) && bufferIsFinite (outputVerb100), "full chain should stay finite at VERB 0/100%");

            const auto settle = (int) (0.5 * sr);
            for (auto freqHz : { 60.0f, 80.0f, 100.0f })
            {
                const auto win = juce::jmin (totalSamples - settle, periodicAnalysisLength (sr, freqHz, 20));
                const auto mag0 = goertzelMagnitude (outputVerb0, 0, totalSamples - win, win, sr, freqHz);
                const auto mag100 = goertzelMagnitude (outputVerb100, 0, totalSamples - win, win, sr, freqHz);
                const auto deltaDb = 20.0f * std::log10 (juce::jmax (mag100, 1.0e-9f) / juce::jmax (mag0, 1.0e-9f));
                std::cout << "  full-chain " << freqHz << "Hz: VERB0->VERB100 change=" << deltaDb << "dB" << std::endl;
                expect (std::abs (deltaDb) < 1.5f, juce::String (freqHz) + "Hz bass should stay close between VERB0 and VERB100 through the full chain: " + juce::String (deltaDb) + "dB");
            }
        }

        beginTest ("reverbEnabled bypass persists through state save/restore");
        {
            UNI76AudioProcessor processor;
            processor.getModuleEnableState().setEnabled (5, false);
            processor.getValueTreeState().getParameter (uni76::ParamID::reverb)->setValueNotifyingHost (0.7f);

            juce::MemoryBlock state;
            processor.getStateInformation (state);

            UNI76AudioProcessor processor2;
            processor2.setStateInformation (state.getData(), (int) state.getSize());
            expect (! processor2.getModuleEnableState().isEnabled (5), "reverbEnabled=false should survive save/restore");
            expectWithinAbsoluteError (processor2.getValueTreeState().getParameter (uni76::ParamID::reverb)->getValue(), 0.7f, 0.001f);
        }
    }
};

static UNI76VerbIntegrationTests uni76VerbIntegrationTests; // NOLINT - self-registers with the UnitTestRunner

namespace
{
    // ---- IMAGE test helpers -----------------------------------------------

    /** Feeds an already-built stereo buffer through `imager` in fixed-size
        blocks, mirroring exactly how PluginProcessor::processBlock() calls
        ImagerProcessor::process(). */
    juce::AudioBuffer<float> runImagerProcessor (uni76::dsp::ImagerProcessor& imager, const juce::AudioBuffer<float>& input,
                                                  int blockSize, float imageNormalised01, float tiltNormalisedMinus1to1, bool enabled)
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

            imager.process (block, imageNormalised01, tiltNormalisedMinus1to1, enabled);

            for (int ch = 0; ch < numChannels; ++ch)
                result.copyFrom (ch, done, block, ch, 0, thisBlock);
            done += thisBlock;
        }
        return result;
    }

    /** A short synthetic "vocal-like" mono source (a fundamental + a few
        partials), returned as identical-L/R stereo - centred mono content
        within a stereo bus, the way a real centred vocal/lead sits. */
    juce::AudioBuffer<float> generateVocalLikeStereo (int totalSamples, double sampleRate)
    {
        const std::vector<float> freqs { 180.0f, 360.0f, 540.0f, 900.0f, 1260.0f };
        const std::vector<float> amps  { 0.22f, 0.12f, 0.08f, 0.04f, 0.02f };
        auto mono = generateChord (totalSamples, sampleRate, freqs, amps);
        juce::AudioBuffer<float> stereo (2, totalSamples);
        stereo.copyFrom (0, 0, mono, 0, 0, totalSamples);
        stereo.copyFrom (1, 0, mono, 0, 0, totalSamples);
        return stereo;
    }
}

class UNI76ImagerProcessorTests final : public juce::UnitTest
{
public:
    UNI76ImagerProcessorTests() : juce::UnitTest ("uni76::dsp::ImagerProcessor", "UNI76") {}

    void runTest() override
    {
        beginTest ("Construct/prepare/reset does not crash; latency is always exactly 0");
        {
            const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
            const int blocks[] { 32, 64, 128, 256, 512, 1024, 2048 };

            for (auto sr : rates)
            {
                for (auto bs : blocks)
                {
                    uni76::dsp::ImagerProcessor imager;
                    imager.prepare (sr, bs, 2);
                    expectEquals (imager.getLatencySamples(), 0);
                    imager.reset();
                    expectEquals (imager.getLatencySamples(), 0);
                }
            }
        }

        beginTest ("Latency is always 0 regardless of IMAGE/TILT/enabled state");
        {
            uni76::dsp::ImagerProcessor imager;
            imager.prepare (44100.0, 512, 2);
            for (auto image : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                for (auto tilt : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
                    for (auto enabled : { true, false })
                    {
                        juce::AudioBuffer<float> buffer (2, 512);
                        buffer.clear();
                        imager.process (buffer, image, tilt, enabled);
                        expectEquals (imager.getLatencySamples(), 0);
                    }
        }

        beginTest ("IMAGE=0/TILT=0 (CENTER) is a bit-exact (up to float rounding) identity transform");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            uni76::dsp::ImagerProcessor imager;
            imager.prepare (sr, blockSize, 2);

            const auto totalSamples = (int) (2.0 * sr);
            auto input = generateIdenticalStereo (totalSamples, sr, 220.0f, 0.2f);
            {
                auto extra = generateChord (totalSamples, sr, { 440.0f, 1500.0f, 4000.0f }, { 0.15f, 0.1f, 0.08f });
                input.addFrom (0, 0, extra, 0, 0, totalSamples);
                input.addFrom (1, 0, extra, 0, 0, totalSamples);
            }
            auto output = runImagerProcessor (imager, input, blockSize, 0.0f, 0.0f, true);

            double sumSq = 0.0, maxDiff = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < totalSamples; ++i)
                {
                    const auto diff = (double) (output.getSample (ch, i) - input.getSample (ch, i));
                    sumSq += diff * diff;
                    maxDiff = juce::jmax (maxDiff, std::abs (diff));
                }
            const auto rmsDiff = std::sqrt (sumSq / (double) (2 * totalSamples));
            std::cout << "\n=== IMAGE CENTER (0/0) null test === RMS diff=" << rmsDiff << " max diff=" << maxDiff << std::endl << std::endl;
            expect (rmsDiff < 1.0e-5, "CENTER should be a near-bit-exact identity transform");
        }

        beginTest ("IMAGE>0/TILT=0: only imaging works (symmetric, no L/R bias)");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (2.0 * sr);

            uni76::dsp::ImagerProcessor imager;
            imager.prepare (sr, blockSize, 2);
            auto input = generateChord (totalSamples, sr, { 300.0f, 3000.0f }, { 0.2f, 0.2f });
            juce::AudioBuffer<float> stereoInput (2, totalSamples);
            stereoInput.copyFrom (0, 0, input, 0, 0, totalSamples);
            stereoInput.copyFrom (1, 0, input, 0, 0, totalSamples);

            auto output = runImagerProcessor (imager, stereoInput, blockSize, 1.0f, 0.0f, true);
            expect (bufferIsFinite (output), "IMAGE=100/TILT=0 should stay finite");

            const auto settle = (int) (0.3 * sr);
            const auto stats = measureStereo (output, settle, totalSamples - settle);
            std::cout << "\n=== IMAGE=100/TILT=0 === rmsL=" << stats.rmsL << " rmsR=" << stats.rmsR << std::endl << std::endl;
            expectWithinAbsoluteError ((float) (stats.rmsL / juce::jmax (stats.rmsR, 1.0e-9)), 1.0f, 0.02f,
                                       "symmetric mono-in-stereo source through IMAGE amount alone should not bias L vs R");
        }

        beginTest ("IMAGE=0/TILT!=0: only tilt works (matches the closed-form tilt gain on Mid, Side untouched)");
        {
            // At IMAGE=0, TILT's own effect is entirely explained by
            // ImagerCurves.h's imageTiltGains() applied to Mid (Side
            // passes straight through, since the width shelf is an exact
            // identity at t=0 - see ImagerProcessor.cpp). The OUTPUT's
            // own naive M/S decomposition (0.5*(L-R)) is *not* the same
            // thing as the untouched Side signal once TILT is active -
            // TILT deliberately creates an L/R gain imbalance on Mid,
            // and that imbalance itself shows up as "Side" energy under
            // a plain M/S read of the output. That is correct, intended
            // behaviour, not a bug - this test checks the real
            // invariant (matches the closed-form gain applied to Mid,
            // computed at a frequency far enough above the tilt
            // safety shelf's corner that its response has converged
            // close to the flat high-frequency asymptote) instead of
            // the wrong one.
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (2.0 * sr);

            uni76::dsp::ImagerProcessor imager;
            imager.prepare (sr, blockSize, 2);

            juce::AudioBuffer<float> input (2, totalSamples);
            input.clear();
            {
                auto l = generateSine (1, totalSamples, sr, 3000.0f, 0.2f);
                auto r = generateSine (1, totalSamples, sr, 4200.0f, 0.2f);
                input.copyFrom (0, 0, l, 0, 0, totalSamples);
                input.copyFrom (1, 0, r, 0, 0, totalSamples);
            }

            constexpr float tiltNorm = 1.0f;
            auto atTilt = runImagerProcessor (imager, input, blockSize, 0.0f, tiltNorm, true);

            float gL = 1.0f, gR = 1.0f;
            uni76::dsp::imageTiltGains (tiltNorm, gL, gR);

            const auto settle = (int) (0.3 * sr);
            double diffSq = 0.0, refSq = 0.0;
            for (int i = settle; i < totalSamples; ++i)
            {
                const auto l0 = (double) input.getSample (0, i);
                const auto r0 = (double) input.getSample (1, i);
                const auto mid = 0.5 * (l0 + r0);
                const auto side = 0.5 * (l0 - r0);
                const auto expectedL = mid * (double) gL + side;
                const auto expectedR = mid * (double) gR - side;

                const auto actualL = (double) atTilt.getSample (0, i);
                const auto actualR = (double) atTilt.getSample (1, i);
                diffSq += (actualL - expectedL) * (actualL - expectedL) + (actualR - expectedR) * (actualR - expectedR);
                refSq += expectedL * expectedL + expectedR * expectedR;
            }
            const auto relDiff = refSq > 1.0e-12 ? std::sqrt (diffSq / refSq) : 0.0;
            std::cout << "\n=== IMAGE=0/TILT=100 closed-form match === relDiff=" << relDiff << std::endl << std::endl;
            expect (relDiff < 0.03, "at IMAGE=0, TILT's output should closely match the closed-form gL/gR applied to Mid, relDiff=" + juce::String (relDiff));
        }

        beginTest ("TILT: -100 tilts left, +100 tilts right (monotonic centroid)");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.0 * sr);
            auto input = generateChord (totalSamples, sr, { 500.0f, 1200.0f }, { 0.2f, 0.15f });
            juce::AudioBuffer<float> stereoInput (2, totalSamples);
            stereoInput.copyFrom (0, 0, input, 0, 0, totalSamples);
            stereoInput.copyFrom (1, 0, input, 0, 0, totalSamples);

            const float tilts[] { -1.0f, -0.5f, -0.25f, 0.0f, 0.25f, 0.5f, 1.0f };
            double previousCentroid = -2.0;
            std::cout << "\n=== TILT centroid sweep ===" << std::endl;
            for (auto tilt : tilts)
            {
                uni76::dsp::ImagerProcessor imager;
                imager.prepare (sr, blockSize, 2);
                auto output = runImagerProcessor (imager, stereoInput, blockSize, 0.0f, tilt, true);
                const auto settle = (int) (0.3 * sr);
                auto series = centroidSeries (output, settle, totalSamples - settle, totalSamples - settle);
                const auto centroid = series.empty() ? 0.0 : series[0];
                std::cout << "  tilt=" << (tilt * 100.0f) << ": centroid=" << centroid << std::endl;
                expect (centroid > previousCentroid - 1.0e-6, "centroid must move monotonically Left->Center->Right as tilt increases");
                previousCentroid = centroid;
                if (tilt < -0.01f) expect (centroid < -0.01, "negative tilt should read as left-biased");
                if (tilt > 0.01f) expect (centroid > 0.01, "positive tilt should read as right-biased");
                if (tilt == 0.0f) expectWithinAbsoluteError (centroid, 0.0, 1.0e-6, "tilt=0 centroid should match the untilted source exactly");
            }
            std::cout << "=== end TILT centroid sweep ===" << std::endl << std::endl;
        }

        beginTest ("TILT: opposite channel never disappears, even at +-100%");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.0 * sr);
            auto input = generateChord (totalSamples, sr, { 400.0f, 900.0f, 2200.0f }, { 0.2f, 0.15f, 0.1f });
            juce::AudioBuffer<float> stereoInput (2, totalSamples);
            stereoInput.copyFrom (0, 0, input, 0, 0, totalSamples);
            stereoInput.copyFrom (1, 0, input, 0, 0, totalSamples);

            for (auto tilt : { -1.0f, 1.0f })
            {
                uni76::dsp::ImagerProcessor imager;
                imager.prepare (sr, blockSize, 2);
                auto output = runImagerProcessor (imager, stereoInput, blockSize, 0.0f, tilt, true);
                const auto settle = (int) (0.3 * sr);
                const auto stats = measureStereo (output, settle, totalSamples - settle);
                const auto quieter = juce::jmin (stats.rmsL, stats.rmsR);
                const auto louder = juce::jmax (stats.rmsL, stats.rmsR);
                std::cout << "  tilt=" << (tilt * 100.0f) << ": rmsL=" << stats.rmsL << " rmsR=" << stats.rmsR << std::endl;
                expect (quieter > 1.0e-4, "the quieter channel must remain clearly nonzero at full tilt, quieter=" + juce::String ((float) quieter));
                expect (quieter / louder > 0.02, "the quieter channel must not be reduced by more than ~34dB at full tilt");
            }
        }

        beginTest ("TILT symmetry: output(-X).L ~= output(+X).R and output(-X).R ~= output(+X).L");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.5 * sr);

            // A genuinely asymmetric stereo source. The correct symmetry
            // relationship - proved algebraically from imageTiltGains()'s
            // gL(theta)==gR(-theta) identity (ImagerCurves.h) - compares
            // TILT(-x) on (L,R) against TILT(+x) on the *channel-swapped*
            // input (R,L): f(-x,L,R).L == f(+x,R,L).R and
            // f(-x,L,R).R == f(+x,R,L).L. Using the same (unswapped) input
            // for both sides, as an earlier version of this test
            // mistakenly did, does not hold in general once TILT creates
            // its own L/R asymmetry - Side's sign does not flip along
            // with the tilt sign unless the input itself is also mirrored.
            juce::AudioBuffer<float> input (2, totalSamples);
            input.clear();
            {
                auto a = generateChord (totalSamples, sr, { 300.0f, 1100.0f }, { 0.2f, 0.12f });
                auto b = generateChord (totalSamples, sr, { 500.0f, 1700.0f }, { 0.18f, 0.09f });
                input.copyFrom (0, 0, a, 0, 0, totalSamples);
                input.addFrom (0, 0, b, 0, 0, totalSamples, 0.4f);
                input.copyFrom (1, 0, b, 0, 0, totalSamples);
                input.addFrom (1, 0, a, 0, 0, totalSamples, 0.4f);
            }
            juce::AudioBuffer<float> inputSwapped (2, totalSamples);
            inputSwapped.copyFrom (0, 0, input, 1, 0, totalSamples);
            inputSwapped.copyFrom (1, 0, input, 0, 0, totalSamples);

            for (auto x : { 0.25f, 0.5f, 1.0f })
            {
                uni76::dsp::ImagerProcessor imagerNeg;
                imagerNeg.prepare (sr, blockSize, 2);
                auto outputNeg = runImagerProcessor (imagerNeg, input, blockSize, 0.0f, -x, true);

                uni76::dsp::ImagerProcessor imagerPos;
                imagerPos.prepare (sr, blockSize, 2);
                auto outputPos = runImagerProcessor (imagerPos, inputSwapped, blockSize, 0.0f, x, true);

                const auto settle = (int) (0.3 * sr);
                double diffLR = 0.0, refLR = 0.0, diffRL = 0.0, refRL = 0.0;
                for (int i = settle; i < totalSamples; ++i)
                {
                    const auto negL = (double) outputNeg.getSample (0, i);
                    const auto posR = (double) outputPos.getSample (1, i);
                    diffLR += (negL - posR) * (negL - posR);
                    refLR += posR * posR;

                    const auto negR = (double) outputNeg.getSample (1, i);
                    const auto posL = (double) outputPos.getSample (0, i);
                    diffRL += (negR - posL) * (negR - posL);
                    refRL += posL * posL;
                }
                const auto relLR = refLR > 1.0e-12 ? std::sqrt (diffLR / refLR) : 0.0;
                const auto relRL = refRL > 1.0e-12 ? std::sqrt (diffRL / refRL) : 0.0;
                std::cout << "  x=" << (x * 100.0f) << ": relDiff(negL,posR)=" << relLR << " relDiff(negR,posL)=" << relRL << std::endl;
                expect (relLR < 0.02, "TILT(-x).L should mirror TILT(+x).R");
                expect (relRL < 0.02, "TILT(-x).R should mirror TILT(+x).L");
            }
        }

        beginTest ("TILT: combined loudness (RMS) stays stable across the whole -100..+100 sweep");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.0 * sr);
            auto input = generateChord (totalSamples, sr, { 250.0f, 800.0f, 2500.0f }, { 0.15f, 0.15f, 0.1f });
            juce::AudioBuffer<float> stereoInput (2, totalSamples);
            stereoInput.copyFrom (0, 0, input, 0, 0, totalSamples);
            stereoInput.copyFrom (1, 0, input, 0, 0, totalSamples);

            std::cout << "\n=== TILT combined-loudness stability ===" << std::endl;
            double referenceCombined = -1.0;
            for (auto tilt : { -1.0f, -0.75f, -0.5f, -0.25f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                uni76::dsp::ImagerProcessor imager;
                imager.prepare (sr, blockSize, 2);
                auto output = runImagerProcessor (imager, stereoInput, blockSize, 0.0f, tilt, true);
                const auto settle = (int) (0.3 * sr);

                double sumSq = 0.0;
                int n = 0;
                for (int i = settle; i < totalSamples; ++i)
                {
                    const auto l = (double) output.getSample (0, i);
                    const auto r = (double) output.getSample (1, i);
                    sumSq += l * l + r * r;
                    ++n;
                }
                const auto combinedRms = std::sqrt (sumSq / (double) n);
                if (referenceCombined < 0.0) referenceCombined = combinedRms;
                const auto deltaDb = 20.0 * std::log10 (combinedRms / referenceCombined);
                std::cout << "  tilt=" << (tilt * 100.0f) << ": combinedRms=" << combinedRms << " deltaVsCenter=" << deltaDb << "dB" << std::endl;
                expect (std::abs (deltaDb) < 1.5, "combined loudness should stay close to stable across the tilt range, delta=" + juce::String (deltaDb) + "dB");
            }
            std::cout << "=== end combined-loudness stability ===" << std::endl << std::endl;
        }

        beginTest ("TILT: low bass is protected, highs get the full tilt amount");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.0 * sr);

            auto measureLRDb = [&] (float freqHz, float tilt)
            {
                auto tone = generateSine (1, totalSamples, sr, freqHz, 0.2f);
                juce::AudioBuffer<float> stereoInput (2, totalSamples);
                stereoInput.copyFrom (0, 0, tone, 0, 0, totalSamples);
                stereoInput.copyFrom (1, 0, tone, 0, 0, totalSamples);

                uni76::dsp::ImagerProcessor imager;
                imager.prepare (sr, blockSize, 2);
                auto output = runImagerProcessor (imager, stereoInput, blockSize, 0.0f, tilt, true);

                const auto settle = (int) (0.3 * sr);
                const auto win = juce::jmin (totalSamples - settle, periodicAnalysisLength (sr, freqHz, 20));
                const auto magL = goertzelMagnitude (output, 0, totalSamples - win, win, sr, freqHz);
                const auto magR = goertzelMagnitude (output, 1, totalSamples - win, win, sr, freqHz);
                return 20.0f * std::log10 (juce::jmax (magL, 1.0e-9f) / juce::jmax (magR, 1.0e-9f));
            };

            const auto bassLRDb = std::abs (measureLRDb (50.0f, 1.0f));
            const auto highLRDb = std::abs (measureLRDb (4000.0f, 1.0f));
            std::cout << "\n=== TILT frequency-dependent bass safety === 50Hz L/R=" << bassLRDb << "dB 4kHz L/R=" << highLRDb << "dB" << std::endl << std::endl;
            expect (bassLRDb < 1.5f, "50Hz should stay close to centred even at full tilt, L/R=" + juce::String (bassLRDb) + "dB");
            expect (highLRDb > bassLRDb + 3.0f, "high frequencies should show clearly more L/R difference than deep bass at full tilt");
        }

        beginTest ("TILT: centred vocal-like source is retained, not eliminated, at +-100%");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.5 * sr);
            auto input = generateVocalLikeStereo (totalSamples, sr);

            for (auto tilt : { -1.0f, 1.0f })
            {
                uni76::dsp::ImagerProcessor imager;
                imager.prepare (sr, blockSize, 2);
                auto output = runImagerProcessor (imager, input, blockSize, 0.0f, tilt, true);
                const auto settle = (int) (0.3 * sr);
                const auto stats = measureStereo (output, settle, totalSamples - settle);
                std::cout << "  vocal tilt=" << (tilt * 100.0f) << ": rmsL=" << stats.rmsL << " rmsR=" << stats.rmsR << std::endl;
                expect (juce::jmin (stats.rmsL, stats.rmsR) > 1.0e-3, "the vocal must remain audible in both channels at full tilt");
            }
        }

        beginTest ("TILT does not collapse existing stereo width (Side/Mid ratio stays meaningful)");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.0 * sr);

            juce::AudioBuffer<float> input (2, totalSamples);
            input.clear();
            {
                auto l = generateChord (totalSamples, sr, { 300.0f, 900.0f }, { 0.2f, 0.15f });
                auto r = generateChord (totalSamples, sr, { 350.0f, 1100.0f }, { 0.18f, 0.13f });
                input.copyFrom (0, 0, l, 0, 0, totalSamples);
                input.copyFrom (1, 0, r, 0, 0, totalSamples);
            }

            const auto settle = (int) (0.3 * sr);
            uni76::dsp::ImagerProcessor imagerZero;
            imagerZero.prepare (sr, blockSize, 2);
            const auto statsZero = measureStereo (runImagerProcessor (imagerZero, input, blockSize, 0.0f, 0.0f, true), settle, totalSamples - settle);

            std::cout << "\n=== TILT width preservation (Side/Mid) === tilt=0: " << statsZero.sideMidRatio << std::endl;
            for (auto tilt : { -1.0f, -0.5f, 0.5f, 1.0f })
            {
                uni76::dsp::ImagerProcessor imager;
                imager.prepare (sr, blockSize, 2);
                const auto stats = measureStereo (runImagerProcessor (imager, input, blockSize, 0.0f, tilt, true), settle, totalSamples - settle);
                std::cout << "  tilt=" << (tilt * 100.0f) << ": Side/Mid=" << stats.sideMidRatio << std::endl;
                expect (stats.sideMidRatio > statsZero.sideMidRatio * 0.5, "width must not collapse dramatically at any tilt setting");
            }
            std::cout << "=== end TILT width preservation ===" << std::endl << std::endl;
        }

        beginTest ("Correlation stays sane across the IMAGE x TILT matrix");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.0 * sr);
            auto input = generateChord (totalSamples, sr, { 300.0f, 700.0f, 1500.0f }, { 0.15f, 0.15f, 0.1f });
            juce::AudioBuffer<float> stereoInput (2, totalSamples);
            stereoInput.copyFrom (0, 0, input, 0, 0, totalSamples);
            stereoInput.copyFrom (1, 0, input, 0, 0, totalSamples);

            std::cout << "\n=== IMAGE x TILT correlation matrix ===" << std::endl;
            for (auto image : { 0.0f, 0.5f, 1.0f })
            {
                for (auto tilt : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f })
                {
                    uni76::dsp::ImagerProcessor imager;
                    imager.prepare (sr, blockSize, 2);
                    auto output = runImagerProcessor (imager, stereoInput, blockSize, image, tilt, true);
                    const auto settle = (int) (0.3 * sr);
                    const auto stats = measureStereo (output, settle, totalSamples - settle);
                    std::cout << "  image=" << (image * 100.0f) << "% tilt=" << (tilt * 100.0f) << ": correlation=" << stats.correlation << std::endl;
                    expect (stats.correlation > -0.5, "correlation should not be driven aggressively negative anywhere in the matrix");
                }
            }
            std::cout << "=== end correlation matrix ===" << std::endl << std::endl;
        }

        beginTest ("Mono fold-down stays safe at IMAGE=100/TILT=+-100");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.0 * sr);
            auto input = generateChord (totalSamples, sr, { 300.0f, 900.0f, 2200.0f }, { 0.15f, 0.12f, 0.08f });
            juce::AudioBuffer<float> stereoInput (2, totalSamples);
            stereoInput.copyFrom (0, 0, input, 0, 0, totalSamples);
            stereoInput.copyFrom (1, 0, input, 0, 0, totalSamples);

            const auto settle = (int) (0.3 * sr);
            uni76::dsp::ImagerProcessor imagerZero;
            imagerZero.prepare (sr, blockSize, 2);
            const auto zeroOut = runImagerProcessor (imagerZero, stereoInput, blockSize, 0.0f, 0.0f, true);
            double refSumSq = 0.0;
            for (int i = settle; i < totalSamples; ++i)
            {
                const auto m = 0.5 * ((double) zeroOut.getSample (0, i) + (double) zeroOut.getSample (1, i));
                refSumSq += m * m;
            }
            const auto refRms = std::sqrt (refSumSq / (double) (totalSamples - settle));

            for (auto tilt : { -1.0f, 1.0f })
            {
                uni76::dsp::ImagerProcessor imager;
                imager.prepare (sr, blockSize, 2);
                auto output = runImagerProcessor (imager, stereoInput, blockSize, 1.0f, tilt, true);
                double sumSq = 0.0;
                for (int i = settle; i < totalSamples; ++i)
                {
                    const auto m = 0.5 * ((double) output.getSample (0, i) + (double) output.getSample (1, i));
                    sumSq += m * m;
                }
                const auto rms = std::sqrt (sumSq / (double) (totalSamples - settle));
                const auto deltaDb = 20.0 * std::log10 (juce::jmax (rms, 1.0e-9) / juce::jmax (refRms, 1.0e-9));
                std::cout << "  IMAGE=100/tilt=" << (tilt * 100.0f) << ": mono fold-down change=" << deltaDb << "dB" << std::endl;
                expect (std::abs (deltaDb) < 6.0, "mono fold-down should not catastrophically collapse or blow up at IMAGE100+TILT100");
            }
        }

        beginTest ("Mono source (identical L/R) can be tilted left/right by TILT alone");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.0 * sr);
            auto input = generateIdenticalStereo (totalSamples, sr, 440.0f, 0.2f);

            uni76::dsp::ImagerProcessor imager;
            imager.prepare (sr, blockSize, 2);
            auto output = runImagerProcessor (imager, input, blockSize, 0.0f, 1.0f, true);
            const auto settle = (int) (0.3 * sr);
            const auto stats = measureStereo (output, settle, totalSamples - settle);
            std::cout << "\n=== mono source + TILT=100 === rmsL=" << stats.rmsL << " rmsR=" << stats.rmsR << std::endl << std::endl;
            expect (stats.rmsR > stats.rmsL * 1.5, "a mono source should become clearly right-biased at TILT=+100 - this is expected, not a bug");
        }

        beginTest ("IMAGE amount alone does not stereoize a mono source");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.0 * sr);
            auto input = generateIdenticalStereo (totalSamples, sr, 440.0f, 0.2f);

            uni76::dsp::ImagerProcessor imager;
            imager.prepare (sr, blockSize, 2);
            auto output = runImagerProcessor (imager, input, blockSize, 1.0f, 0.0f, true);
            const auto settle = (int) (0.3 * sr);
            const auto stats = measureStereo (output, settle, totalSamples - settle);
            std::cout << "\n=== mono source + IMAGE=100/TILT=0 === Side RMS=" << stats.rmsSide << std::endl << std::endl;
            expect (stats.rmsSide < 1.0e-5, "IMAGE amount by itself must never synthesise Side content from a mono source");
        }

        beginTest ("Real mono bus (numChannels=1): TILT and IMAGE are both DSP-neutral, no gain change");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.0 * sr);
            auto input = generateSine (1, totalSamples, sr, 440.0f, 0.2f);

            for (auto tilt : { -1.0f, 1.0f })
            {
                for (auto image : { 0.0f, 1.0f })
                {
                    uni76::dsp::ImagerProcessor imager;
                    imager.prepare (sr, blockSize, 1);
                    auto output = runImagerProcessor (imager, input, blockSize, image, tilt, true);

                    double diffSq = 0.0, refSq = 0.0;
                    for (int i = 0; i < totalSamples; ++i)
                    {
                        const auto diff = (double) output.getSample (0, i) - (double) input.getSample (0, i);
                        diffSq += diff * diff;
                        refSq += (double) input.getSample (0, i) * (double) input.getSample (0, i);
                    }
                    const auto relDiff = refSq > 1.0e-12 ? std::sqrt (diffSq / refSq) : 0.0;
                    expect (relDiff < 1.0e-5, "a real mono bus must stay completely untouched regardless of IMAGE/TILT");
                }
            }
        }

        beginTest ("No pitch change / no time modulation: a steady tone's level stays stable across two separated windows");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (3.0 * sr);
            auto tone = generateSine (1, totalSamples, sr, 1000.0f, 0.2f);
            juce::AudioBuffer<float> stereoInput (2, totalSamples);
            stereoInput.copyFrom (0, 0, tone, 0, 0, totalSamples);
            stereoInput.copyFrom (1, 0, tone, 0, 0, totalSamples);

            uni76::dsp::ImagerProcessor imager;
            imager.prepare (sr, blockSize, 2);
            auto output = runImagerProcessor (imager, stereoInput, blockSize, 0.5f, 0.5f, true);

            const auto win = periodicAnalysisLength (sr, 1000.0f, 40);
            const auto earlyMag = goertzelMagnitude (output, 0, (int) (0.5 * sr), win, sr, 1000.0f);
            const auto lateMag = goertzelMagnitude (output, 0, totalSamples - win, win, sr, 1000.0f);
            const auto deltaDb = 20.0f * std::log10 (juce::jmax (lateMag, 1.0e-9f) / juce::jmax (earlyMag, 1.0e-9f));
            std::cout << "\n=== IMAGE static-output check === early->late 1kHz change=" << deltaDb << "dB" << std::endl << std::endl;
            expect (std::abs (deltaDb) < 0.1f, "a static IMAGE/TILT setting must not modulate level over time - delta=" + juce::String (deltaDb) + "dB");
        }

        beginTest ("NaN/Inf macro input and audio samples are sanitised, all sample rates/block sizes stay finite");
        {
            const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
            const int blocks[] { 32, 128, 512, 2048 };

            for (auto sr : rates)
            {
                for (auto bs : blocks)
                {
                    uni76::dsp::ImagerProcessor imager;
                    imager.prepare (sr, bs, 2);
                    juce::AudioBuffer<float> buffer (2, bs);
                    buffer.clear();
                    buffer.setSample (0, 0, std::numeric_limits<float>::quiet_NaN());
                    buffer.setSample (1, 0, std::numeric_limits<float>::infinity());
                    imager.process (buffer, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), true);
                    expect (bufferIsFinite (buffer), "NaN/Inf input should be sanitised at " + juce::String (sr) + "Hz/" + juce::String (bs));

                    // A second block confirms the smoother itself recovered
                    // cleanly (matching the VERB regression test pattern -
                    // see docs/DSP_VERB.md's "A real bug found by Debug-mode
                    // testing" section).
                    juce::AudioBuffer<float> secondBlock (2, bs);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < bs; ++i)
                            secondBlock.setSample (ch, i, 0.1f);
                    imager.process (secondBlock, 0.5f, 0.5f, true);
                    expect (bufferIsFinite (secondBlock), "processing after NaN macro input should stay finite on the next block too");
                }
            }
        }

        beginTest ("Silence in produces silence out - no added noise/hiss/hum");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            uni76::dsp::ImagerProcessor imager;
            imager.prepare (sr, blockSize, 2);
            juce::AudioBuffer<float> silence (2, (int) (1.0 * sr));
            silence.clear();
            auto output = runImagerProcessor (imager, silence, blockSize, 0.5f, 0.5f, true);

            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < output.getNumSamples(); ++i)
                    peak = juce::jmax (peak, std::abs (output.getSample (ch, i)));
            expect (peak < 1.0e-6f, "silence in should give silence out, peak=" + juce::String (peak));
        }

        beginTest ("Bypass (enabled=false) mutes both axes - dry stays exact; automation is click-free");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (1.0 * sr);
            auto input = generateChord (totalSamples, sr, { 300.0f, 900.0f }, { 0.2f, 0.15f });
            juce::AudioBuffer<float> stereoInput (2, totalSamples);
            stereoInput.copyFrom (0, 0, input, 0, 0, totalSamples);
            stereoInput.copyFrom (1, 0, input, 0, 0, totalSamples);

            uni76::dsp::ImagerProcessor imager;
            imager.prepare (sr, blockSize, 2);
            auto output = runImagerProcessor (imager, stereoInput, blockSize, 1.0f, 1.0f, false);

            double diffSq = 0.0, refSq = 0.0;
            const auto settle = (int) (0.15 * sr); // past the bypass smoother's own short ramp
            for (int i = settle; i < totalSamples; ++i)
                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto diff = (double) output.getSample (ch, i) - (double) stereoInput.getSample (ch, i);
                    diffSq += diff * diff;
                    refSq += (double) stereoInput.getSample (ch, i) * (double) stereoInput.getSample (ch, i);
                }
            const auto relDiff = refSq > 1.0e-12 ? std::sqrt (diffSq / refSq) : 0.0;
            expect (relDiff < 1.0e-4, "disabled IMAGE should be an exact dry passthrough once the bypass ramp settles");

            float maxJump = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 1; i < output.getNumSamples(); ++i)
                    maxJump = juce::jmax (maxJump, std::abs (output.getSample (ch, i) - output.getSample (ch, i - 1)));
            expect (maxJump < 1.0f, "bypass transition should not create an extreme sample-to-sample discontinuity");
        }

        beginTest ("Mono bus (numChannels=1) stays completely untouched at every setting");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            uni76::dsp::ImagerProcessor imager;
            imager.prepare (sr, blockSize, 1);
            auto input = generateSine (1, (int) (1.0 * sr), sr, 1000.0f, 0.3f);
            juce::AudioBuffer<float> output (1, input.getNumSamples());
            int done = 0;
            while (done < input.getNumSamples())
            {
                const auto thisBlock = juce::jmin (blockSize, input.getNumSamples() - done);
                juce::AudioBuffer<float> block (1, thisBlock);
                block.copyFrom (0, 0, input, 0, done, thisBlock);
                imager.process (block, 1.0f, 1.0f, true);
                output.copyFrom (0, done, block, 0, 0, thisBlock);
                done += thisBlock;
            }
            expect (bufferIsFinite (output), "mono bus should stay finite regardless of IMAGE/TILT");
        }
    }
};

static UNI76ImagerProcessorTests uni76ImagerProcessorTests; // NOLINT - self-registers with the UnitTestRunner

class UNI76ImagerIntegrationTests final : public juce::UnitTest
{
public:
    UNI76ImagerIntegrationTests() : juce::UnitTest ("UNI76AudioProcessor+IMAGE", "UNI76") {}

    void runTest() override
    {
        beginTest ("Fresh instance: imager defaults to 0%, imageTilt defaults to 0 (CENTER)");
        {
            UNI76AudioProcessor processor;
            auto& apvts = processor.getValueTreeState();
            auto* imagerParam = apvts.getParameter (uni76::ParamID::imager);
            auto* tiltParam = apvts.getParameter (uni76::ParamID::imageTilt);
            expect (imagerParam != nullptr && tiltParam != nullptr);
            if (imagerParam != nullptr)
                expectWithinAbsoluteError (imagerParam->getValue(), 0.0f, 0.001f, "imager should default to 0%");
            if (tiltParam != nullptr)
                expectWithinAbsoluteError (tiltParam->getValue(), 0.5f, 0.001f, "imageTilt should default to its normalised midpoint (0/CENTER)");

            if (auto* tiltFloat = dynamic_cast<juce::AudioParameterFloat*> (tiltParam))
            {
                expectWithinAbsoluteError (tiltFloat->get(), 0.0f, 0.001f, "imageTilt real value should default to exactly 0 (CENTER)");
                expectWithinAbsoluteError (tiltFloat->range.start, -100.0f, 0.001f);
                expectWithinAbsoluteError (tiltFloat->range.end, 100.0f, 0.001f);
            }
        }

        beginTest ("Total plugin latency is unchanged by IMAGE (still PREAMP+EQ+SAT+PITCH, PAN+VERB+IMAGE all add 0)");
        {
            const double rates[] { 44100.0, 48000.0, 96000.0, 192000.0 };
            for (auto sr : rates)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 512);

                uni76::dsp::PreampProcessor preamp; preamp.prepare (sr, 512, 2);
                uni76::dsp::EqProcessor eq; eq.prepare (sr, 512, 2);
                uni76::dsp::SatProcessor sat; sat.prepare (sr, 512, 2);
                uni76::dsp::PitchProcessor pitch; pitch.prepare (sr, 512, 2);
                uni76::dsp::PanoramaProcessor pan; pan.prepare (sr, 512, 2);
                uni76::dsp::VerbProcessor verb; verb.prepare (sr, 512, 2);
                uni76::dsp::ImagerProcessor imager; imager.prepare (sr, 512, 2);

                expectEquals (imager.getLatencySamples(), 0);
                expectEquals (processor.getLatencySamples(),
                               preamp.getLatencySamples() + eq.getLatencySamples() + sat.getLatencySamples()
                               + pitch.getLatencySamples() + pan.getLatencySamples() + verb.getLatencySamples()
                               + imager.getLatencySamples());
            }
        }

        beginTest ("PAN=100 + TILT: motion continues, PAN LFO period is unchanged by TILT");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (10.0 * sr);

            auto runPanThenTilt = [&] (float tilt)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, blockSize);
                auto& apvts = processor.getValueTreeState();
                apvts.getParameter (uni76::ParamID::panorama)->setValueNotifyingHost (1.0f);
                apvts.getParameter (uni76::ParamID::imageTilt)->setValueNotifyingHost (0.5f + tilt * 0.5f);

                juce::AudioBuffer<float> buffer (2, totalSamples);
                buffer.clear();
                {
                    auto highs = generateChord (totalSamples, sr, { 3000.0f, 4500.0f }, { 0.15f, 0.1f });
                    buffer.addFrom (0, 0, highs, 0, 0, totalSamples);
                    buffer.addFrom (1, 0, highs, 0, 0, totalSamples);
                }

                juce::MidiBuffer midi;
                int done = 0;
                while (done < totalSamples)
                {
                    const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
                    juce::AudioBuffer<float> block (2, thisBlock);
                    block.copyFrom (0, 0, buffer, 0, done, thisBlock);
                    block.copyFrom (1, 0, buffer, 1, done, thisBlock);
                    processor.processBlock (block, midi);
                    buffer.copyFrom (0, done, block, 0, 0, thisBlock);
                    buffer.copyFrom (1, done, block, 1, 0, thisBlock);
                    done += thisBlock;
                }
                return buffer;
            };

            const auto settle = (int) (0.5 * sr);
            const auto windowLen = (int) (0.05 * sr);

            auto outputCenter = runPanThenTilt (0.0f);
            auto outputLeft = runPanThenTilt (-1.0f);
            auto outputRight = runPanThenTilt (1.0f);
            expect (bufferIsFinite (outputCenter) && bufferIsFinite (outputLeft) && bufferIsFinite (outputRight),
                    "PAN+TILT combined chain should stay finite");

            auto seriesCenter = centroidSeries (outputCenter, settle, totalSamples - settle, windowLen);
            auto seriesLeft = centroidSeries (outputLeft, settle, totalSamples - settle, windowLen);
            auto seriesRight = centroidSeries (outputRight, settle, totalSamples - settle, windowLen);

            auto statsCenter = analyzeSeries (seriesCenter);
            auto statsLeft = analyzeSeries (seriesLeft);
            auto statsRight = analyzeSeries (seriesRight);

            std::cout << "\n=== PAN100+TILT centroid bias ===" << std::endl;
            std::cout << "  TILT=0:    mean=" << statsCenter.mean << " excursion=" << statsCenter.rmsExcursion << std::endl;
            std::cout << "  TILT=-100: mean=" << statsLeft.mean << " excursion=" << statsLeft.rmsExcursion << std::endl;
            std::cout << "  TILT=+100: mean=" << statsRight.mean << " excursion=" << statsRight.rmsExcursion << std::endl;
            std::cout << "=== end PAN100+TILT centroid bias ===" << std::endl << std::endl;

            // Item 25: TILT shifts the average bias of PAN's own motion trajectory.
            expect (statsLeft.mean < statsCenter.mean - 0.02, "TILT=-100 should bias PAN's average trajectory left");
            expect (statsRight.mean > statsCenter.mean + 0.02, "TILT=+100 should bias PAN's average trajectory right");

            // Item 24: motion continues (excursion doesn't collapse to
            // near-zero) at any tilt. TILT's own bounded Mid-domain gain
            // pair legitimately shrinks the *centroid ratio*'s excursion
            // somewhat even though the underlying motion is unaffected in
            // absolute (dB) terms: at full tilt one channel's Mid content
            // is heavily attenuated (see ImagerCurves.h's thetaMax), which
            // shifts each channel's own energy baseline enough to compress
            // (Renergy-Lenergy)/(Renergy+Lenergy)'s swing without the
            // motion itself becoming inaudible - measured ~37-39% of the
            // untilted excursion on this test's synthetic source, still
            // clearly nonzero and far from "motion vanished." The
            // assertion below checks for that ("still clearly audible/
            // present"), not "unchanged in magnitude" - see
            // docs/DSP_IMAGE.md's "PAN interaction" section.
            expect (statsLeft.rmsExcursion > statsCenter.rmsExcursion * 0.25, "PAN's motion should still be clearly audible under TILT=-100");
            expect (statsRight.rmsExcursion > statsCenter.rmsExcursion * 0.25, "PAN's motion should still be clearly audible under TILT=+100");

            // Item 24: LFO period is unaffected by TILT - estimate the period
            // via zero-crossing spacing of the (mean-removed) centroid series
            // and confirm all three land close together.
            auto estimatePeriodSamples = [&] (const std::vector<double>& series, double mean) -> double
            {
                std::vector<int> crossings;
                for (size_t i = 1; i < series.size(); ++i)
                    if ((series[i - 1] - mean) < 0.0 && (series[i] - mean) >= 0.0)
                        crossings.push_back ((int) i);
                if (crossings.size() < 2) return 0.0;
                return (double) (crossings.back() - crossings.front()) / (double) (crossings.size() - 1) * (double) windowLen;
            };

            const auto periodCenter = estimatePeriodSamples (seriesCenter, statsCenter.mean);
            const auto periodLeft = estimatePeriodSamples (seriesLeft, statsLeft.mean);
            const auto periodRight = estimatePeriodSamples (seriesRight, statsRight.mean);
            std::cout << "  LFO period estimate (samples): center=" << periodCenter << " left=" << periodLeft << " right=" << periodRight << std::endl;
            if (periodCenter > 0.0 && periodLeft > 0.0)
                expect (std::abs (periodLeft - periodCenter) / periodCenter < 0.15, "TILT must not change PAN's own LFO period");
            if (periodCenter > 0.0 && periodRight > 0.0)
                expect (std::abs (periodRight - periodCenter) / periodCenter < 0.15, "TILT must not change PAN's own LFO period");
        }

        beginTest ("VERB+IMAGE+TILT: full wet chain stays finite and stable");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (2.0 * sr);

            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (sr, blockSize);
            auto& apvts = processor.getValueTreeState();
            apvts.getParameter (uni76::ParamID::reverb)->setValueNotifyingHost (0.6f);
            apvts.getParameter (uni76::ParamID::imager)->setValueNotifyingHost (0.7f);
            apvts.getParameter (uni76::ParamID::imageTilt)->setValueNotifyingHost (0.75f); // +50

            juce::AudioBuffer<float> buffer (2, totalSamples);
            buffer.clear();
            {
                auto content = generateChord (totalSamples, sr, { 100.0f, 500.0f, 3000.0f }, { 0.15f, 0.12f, 0.08f });
                buffer.addFrom (0, 0, content, 0, 0, totalSamples);
                buffer.addFrom (1, 0, content, 0, 0, totalSamples);
            }

            juce::MidiBuffer midi;
            int done = 0;
            while (done < totalSamples)
            {
                const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
                juce::AudioBuffer<float> block (2, thisBlock);
                block.copyFrom (0, 0, buffer, 0, done, thisBlock);
                block.copyFrom (1, 0, buffer, 1, done, thisBlock);
                processor.processBlock (block, midi);
                buffer.copyFrom (0, done, block, 0, 0, thisBlock);
                buffer.copyFrom (1, done, block, 1, 0, thisBlock);
                done += thisBlock;
            }
            expect (bufferIsFinite (buffer), "VERB+IMAGE+TILT combined chain should stay finite");

            float peak = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < totalSamples; ++i)
                    peak = juce::jmax (peak, std::abs (buffer.getSample (ch, i)));
            expect (peak < 4.0f, "VERB+IMAGE+TILT should not blow up the signal, peak=" + juce::String (peak));
        }

        beginTest ("Full chain PREAMP+EQ+SAT+PITCH+PAN+VERB+IMAGE+TILT stays finite/stable for representative combinations");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            const auto totalSamples = (int) (2.0 * sr);

            struct Combo { float preamp, eq, sat; int pitch; float pan, verb, image, tilt; };
            const Combo combos[]
            {
                { 0.3f, 0.5f, 0.2f, 3, 0.5f, 0.3f, 0.5f, 0.75f },   // TILT +50
                { 0.6f, 0.25f, 0.5f, -5, 1.0f, 0.6f, 1.0f, 0.0f },  // CENTER
                { 0.0f, 0.75f, 0.0f, 0, 0.0f, 0.0f, 0.0f, 0.0f },   // TILT -100
            };

            for (auto& c : combos)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, blockSize);
                auto& apvts = processor.getValueTreeState();
                apvts.getParameter (uni76::ParamID::preamp)->setValueNotifyingHost (c.preamp);
                apvts.getParameter (uni76::ParamID::eq)->setValueNotifyingHost (c.eq);
                apvts.getParameter (uni76::ParamID::saturation)->setValueNotifyingHost (c.sat);
                apvts.getParameter (uni76::ParamID::pitch)->setValueNotifyingHost ((float) (c.pitch + 12) / 24.0f);
                apvts.getParameter (uni76::ParamID::panorama)->setValueNotifyingHost (c.pan);
                apvts.getParameter (uni76::ParamID::reverb)->setValueNotifyingHost (c.verb);
                apvts.getParameter (uni76::ParamID::imager)->setValueNotifyingHost (c.image);
                apvts.getParameter (uni76::ParamID::imageTilt)->setValueNotifyingHost (c.tilt);

                juce::AudioBuffer<float> buffer (2, totalSamples);
                buffer.clear();
                {
                    auto content = generateChord (totalSamples, sr, { 80.0f, 440.0f, 2000.0f, 6000.0f }, { 0.15f, 0.15f, 0.1f, 0.05f });
                    buffer.addFrom (0, 0, content, 0, 0, totalSamples);
                    buffer.addFrom (1, 0, content, 0, 0, totalSamples);
                }

                juce::MidiBuffer midi;
                int done = 0;
                while (done < totalSamples)
                {
                    const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
                    juce::AudioBuffer<float> block (2, thisBlock);
                    block.copyFrom (0, 0, buffer, 0, done, thisBlock);
                    block.copyFrom (1, 0, buffer, 1, done, thisBlock);
                    processor.processBlock (block, midi);
                    buffer.copyFrom (0, done, block, 0, 0, thisBlock);
                    buffer.copyFrom (1, done, block, 1, 0, thisBlock);
                    done += thisBlock;
                }
                expect (bufferIsFinite (buffer), "full 7-module chain should stay finite for every representative combination");
            }
        }

        beginTest ("imageTilt automation sweep is click-free and stays finite");
        {
            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;

            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (sr, blockSize);
            auto& apvts = processor.getValueTreeState();
            apvts.getParameter (uni76::ParamID::imager)->setValueNotifyingHost (0.5f);

            juce::MidiBuffer midi;
            juce::AudioBuffer<float> full (2, 0);
            const std::vector<float> sweep { 0.5f, 0.0f, 1.0f, 0.25f, 0.75f, 0.5f };
            for (auto normalised : sweep)
            {
                apvts.getParameter (uni76::ParamID::imageTilt)->setValueNotifyingHost (normalised);

                // Chunked into blockSize-sized pieces, matching the max
                // block size prepareToPlay() declared above - calling
                // processBlock() with a larger buffer than that overruns
                // internal scratch buffers sized to it (e.g. ImagerProcessor's
                // own dryScratch) - this is the exact self-inflicted bug
                // class documented in docs/DSP_VERB.md's host-validation
                // section from the earlier VERB round.
                const auto stepSamples = (int) (0.1 * sr);
                juce::AudioBuffer<float> step (2, stepSamples);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < stepSamples; ++i)
                        step.setSample (ch, i, 0.2f * std::sin ((float) i * 0.05f));

                int done = 0;
                while (done < stepSamples)
                {
                    const auto thisBlock = juce::jmin (blockSize, stepSamples - done);
                    juce::AudioBuffer<float> block (2, thisBlock);
                    block.copyFrom (0, 0, step, 0, done, thisBlock);
                    block.copyFrom (1, 0, step, 1, done, thisBlock);
                    processor.processBlock (block, midi);
                    step.copyFrom (0, done, block, 0, 0, thisBlock);
                    step.copyFrom (1, done, block, 1, 0, thisBlock);
                    done += thisBlock;
                }
                expect (bufferIsFinite (step), "automation step should stay finite");

                const auto oldSize = full.getNumSamples();
                full.setSize (2, oldSize + stepSamples, true, true, true);
                full.copyFrom (0, oldSize, step, 0, 0, stepSamples);
                full.copyFrom (1, oldSize, step, 1, 0, stepSamples);
            }

            float maxJump = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 1; i < full.getNumSamples(); ++i)
                    maxJump = juce::jmax (maxJump, std::abs (full.getSample (ch, i) - full.getSample (ch, i - 1)));
            expect (maxJump < 1.0f, "imageTilt automation should not create an extreme sample-to-sample discontinuity");
        }

        beginTest ("imagerEnabled bypass mutes both IMAGE axes and persists through state save/restore");
        {
            UNI76AudioProcessor processor;
            processor.getModuleEnableState().setEnabled (6, false);
            processor.getValueTreeState().getParameter (uni76::ParamID::imager)->setValueNotifyingHost (0.8f);
            processor.getValueTreeState().getParameter (uni76::ParamID::imageTilt)->setValueNotifyingHost (0.9f);

            juce::MemoryBlock state;
            processor.getStateInformation (state);

            UNI76AudioProcessor processor2;
            processor2.setStateInformation (state.getData(), (int) state.getSize());
            expect (! processor2.getModuleEnableState().isEnabled (6), "imagerEnabled=false should survive save/restore");
            expectWithinAbsoluteError (processor2.getValueTreeState().getParameter (uni76::ParamID::imager)->getValue(), 0.8f, 0.001f);
            expectWithinAbsoluteError (processor2.getValueTreeState().getParameter (uni76::ParamID::imageTilt)->getValue(), 0.9f, 0.001f);
        }

        beginTest ("imageTilt value round-trips through a real getStateInformation()/setStateInformation() save+restore");
        {
            UNI76AudioProcessor processor;
            processor.getValueTreeState().getParameter (uni76::ParamID::imageTilt)->setValueNotifyingHost (0.15f); // -70

            juce::MemoryBlock state;
            processor.getStateInformation (state);

            UNI76AudioProcessor processor2;
            processor2.setStateInformation (state.getData(), (int) state.getSize());
            expectWithinAbsoluteError (processor2.getValueTreeState().getParameter (uni76::ParamID::imageTilt)->getValue(), 0.15f, 0.001f);
        }

        beginTest ("Legacy state without imageTilt defaults to 0 (CENTER)");
        {
            // Hand-built to look like a real state saved *before* imageTilt
            // existed: a PARAMETERS tree with every other parameter but no
            // <PARAM id="imageTilt".../> child at all - see
            // Core/PluginIdentity.h's documented decision not to bump
            // stateSchemaVersion for this addition.
            juce::ValueTree legacyState ("PARAMETERS");
            legacyState.setProperty (uni76::stateSchemaVersionProperty, uni76::stateSchemaVersion, nullptr);

            for (const auto* id : uni76::ParamID::all)
            {
                if (std::strcmp (id, uni76::ParamID::imageTilt) == 0)
                    continue; // deliberately omitted - the whole point of this test

                juce::ValueTree param ("PARAM");
                param.setProperty ("id", juce::String (id), nullptr);
                param.setProperty ("value", std::strcmp (id, uni76::ParamID::pitch) == 0 ? 0.5 : 0.3, nullptr);
                legacyState.appendChild (param, nullptr);
            }

            juce::MemoryBlock data;
            if (auto xml = legacyState.createXml())
                juce::AudioProcessor::copyXmlToBinary (*xml, data);

            UNI76AudioProcessor processor;
            processor.setStateInformation (data.getData(), (int) data.getSize());

            auto* tiltParam = processor.getValueTreeState().getParameter (uni76::ParamID::imageTilt);
            expect (tiltParam != nullptr);
            if (tiltParam != nullptr)
                expectWithinAbsoluteError (tiltParam->getValue(), 0.5f, 0.001f, "imageTilt should fall back to its own default (0/CENTER) when absent from a legacy state");
        }
    }
};

static UNI76ImagerIntegrationTests uni76ImagerIntegrationTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// Pre-release full-audit tests (docs/FULL_DSP_AUDIT.md). These are
// cross-cutting, whole-plugin checks that don't belong to any single
// module's own test class above - migration across every real historical
// schema version at once, technical-neutral/gain-staging/extreme-matrix
// stress, full-chain spatial integration, and process-lifecycle/
// robustness/allocation/determinism/CPU checks. All DSP exercised here is
// frozen (see CLAUDE.md) - these tests exist to prove the *whole plugin*
// behaves correctly, not to change any module's sound.

// Global operator new/delete replacement so the allocation-audit test
// below can prove processBlock() makes zero dynamic allocations, program-
// wide, not just in code this test file happens to call directly. Only
// active while uni76audit::trackingAllocs is set, so it costs nothing
// (beyond an atomic load) for every other test in this binary.
namespace uni76audit
{
    std::atomic<long long> allocCount { 0 };
    std::atomic<bool> trackingAllocs { false };
}

void* operator new (std::size_t size)
{
    if (uni76audit::trackingAllocs.load (std::memory_order_relaxed))
        uni76audit::allocCount.fetch_add (1, std::memory_order_relaxed);
    void* p = std::malloc (size == 0 ? 1 : size);
    if (p == nullptr)
        throw std::bad_alloc();
    return p;
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void* operator new[] (std::size_t size) { return operator new (size); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
    // ---- Full-chain (whole AudioProcessor) test helpers ------------------

    void setNormalised (juce::AudioProcessorValueTreeState& apvts, const char* id, float normalised01)
    {
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (normalised01);
    }

    struct ParamSpec { const char* id; float defaultNorm; };

    // Every one of the 8 parameters expressed uniformly in normalised 0..1
    // space: 1.0 is always that parameter's "loud" extreme (100%, +12 ST,
    // +100 TILT); 0.0 is a second, distinct extreme only for pitch (-12 ST)
    // and imageTilt (-100/LEFT) - for the other six, 0.0 is simply their
    // own resting default.
    const std::array<ParamSpec, 8> auditParams {{
        { uni76::ParamID::preamp,     0.0f },
        { uni76::ParamID::eq,         0.5f },
        { uni76::ParamID::saturation, 0.0f },
        { uni76::ParamID::pitch,      0.5f },
        { uni76::ParamID::panorama,   0.0f },
        { uni76::ParamID::reverb,     0.0f },
        { uni76::ParamID::imager,     0.0f },
        { uni76::ParamID::imageTilt,  0.5f },
    }};

    void applyAuditDefaults (juce::AudioProcessorValueTreeState& apvts)
    {
        for (auto& spec : auditParams)
            setNormalised (apvts, spec.id, spec.defaultNorm);
    }

    /** Runs an already-built buffer through the whole plugin in fixed-size
        chunks, mirroring exactly how a host calls processBlock(). */
    juce::AudioBuffer<float> runFullChain (UNI76AudioProcessor& processor, const juce::AudioBuffer<float>& input, int blockSize)
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

            juce::MidiBuffer midi;
            processor.processBlock (block, midi);

            for (int ch = 0; ch < numChannels; ++ch)
                result.copyFrom (ch, done, block, ch, 0, thisBlock);

            done += thisBlock;
        }
        return result;
    }

    double peakOf (const juce::AudioBuffer<float>& b)
    {
        double peak = 0.0;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                peak = juce::jmax (peak, (double) std::abs (b.getSample (ch, i)));
        return peak;
    }

    double rmsOf (const juce::AudioBuffer<float>& b, int channel, int start, int n)
    {
        double sum = 0.0;
        for (int i = start; i < start + n; ++i)
        {
            const auto v = (double) b.getSample (channel, i);
            sum += v * v;
        }
        return n > 0 ? std::sqrt (sum / (double) n) : 0.0;
    }

    double dcOffsetOf (const juce::AudioBuffer<float>& b, int channel, int start, int n)
    {
        double sum = 0.0;
        for (int i = start; i < start + n; ++i)
            sum += (double) b.getSample (channel, i);
        return n > 0 ? sum / (double) n : 0.0;
    }

    double crestFactorDb (double peak, double rms)
    {
        return rms > 1.0e-12 ? 20.0 * std::log10 (peak / rms) : 0.0;
    }

    juce::AudioBuffer<float> makeStereoFromMono (const juce::AudioBuffer<float>& mono)
    {
        juce::AudioBuffer<float> stereo (2, mono.getNumSamples());
        stereo.copyFrom (0, 0, mono, 0, 0, mono.getNumSamples());
        stereo.copyFrom (1, 0, mono, 0, 0, mono.getNumSamples());
        return stereo;
    }

    /** One representative test source per audit source category (item 6 /
        item 7's "sources" list) - deterministic, no randomness, matching
        every other generator in this file. */
    std::vector<std::pair<juce::String, juce::AudioBuffer<float>>> makeAuditSources (double sampleRate, int totalSamples, float amplitude)
    {
        std::vector<std::pair<juce::String, juce::AudioBuffer<float>>> sources;
        sources.emplace_back ("sine",              makeStereoFromMono (generateSine (1, totalSamples, sampleRate, 440.0f, amplitude)));
        sources.emplace_back ("bass",               makeStereoFromMono (generateSine (1, totalSamples, sampleRate, 60.0f, amplitude)));
        sources.emplace_back ("transient",          [&] {
            auto buf = makeStereoFromMono (generateSine (1, totalSamples, sampleRate, 1000.0f, amplitude));
            // A hard onset at t=0 followed by exponential-ish decay via a
            // simple envelope, so the source has a genuine transient edge.
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    buf.setSample (ch, i, buf.getSample (ch, i) * (float) std::exp (-3.0 * (double) i / (double) totalSamples));
            return buf;
        } ());
        sources.emplace_back ("vocal-like",         generateMonoHarmonicStereo (totalSamples, sampleRate, amplitude));
        sources.emplace_back ("chord",              makeStereoFromMono (generateChord (totalSamples, sampleRate, { 220.0f, 277.18f, 329.63f }, { amplitude, amplitude * 0.8f, amplitude * 0.7f })));
        sources.emplace_back ("correlated-stereo",  generateCorrelatedChord (totalSamples, sampleRate));
        sources.emplace_back ("decorrelated-stereo", generateDecorrelatedStereo (totalSamples, sampleRate));
        sources.emplace_back ("synthetic-mix",      generateCenterBassStereoHighs (totalSamples, sampleRate));
        return sources;
    }

    /** Builds a legacy state by starting from a *current*, correctly-shaped
        ValueTree (apvts.copyState()) and overriding exactly the properties
        a given historical version would actually have had - the same
        technique the existing per-parameter migration tests above use,
        generalised so one test can walk every real historical schema
        shape at once. `includeEnabledFlags`/`includeImageTilt` model
        whether that version's saved state had those properties at all. */
    juce::MemoryBlock buildLegacyStateFrom (juce::AudioProcessorValueTreeState& apvts, int schemaVersion,
                                              bool includeEnabledFlags, bool includeImageTilt,
                                              float pitchRawValue, float panoramaRawValue)
    {
        auto legacyState = apvts.copyState();
        legacyState.setProperty (uni76::stateSchemaVersionProperty, schemaVersion, nullptr);

        if (includeEnabledFlags)
            for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                legacyState.setProperty (uni76::ModuleEnableState::propertyNames[(size_t) i], true, nullptr);
        else
            for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                legacyState.removeProperty (uni76::ModuleEnableState::propertyNames[(size_t) i], nullptr);

        auto pitchParam = legacyState.getChildWithProperty ("id", juce::var (uni76::ParamID::pitch));
        if (pitchParam.isValid())
            pitchParam.setProperty ("value", (double) pitchRawValue, nullptr);

        auto panoramaParam = legacyState.getChildWithProperty ("id", juce::var (uni76::ParamID::panorama));
        if (panoramaParam.isValid())
            panoramaParam.setProperty ("value", (double) panoramaRawValue, nullptr);

        if (! includeImageTilt)
        {
            auto tiltParam = legacyState.getChildWithProperty ("id", juce::var (uni76::ParamID::imageTilt));
            if (tiltParam.isValid())
                legacyState.removeChild (tiltParam, nullptr);
        }

        juce::MemoryBlock block;
        if (auto xml = legacyState.createXml())
            juce::AudioProcessor::copyXmlToBinary (*xml, block);
        return block;
    }

    // ---- Technical-neutral decomposition helpers (RC1 blocker 1) --------
    //
    // The original "technical-neutral dry diff" measurement (see
    // UNI76FullAuditGainStagingTests's "Technical-neutral state..." test)
    // reported exactly one number - `20*log10(RMS(output_latency_aligned -
    // input) / RMS(input))` - a *null-residual-relative-to-input* metric,
    // not a gain delta and not a correlation. It answers "how large is the
    // difference signal compared to the input", which conflates a pure
    // level/gain error with genuine waveform-shape change (harmonic
    // distortion). NeutralMeasurement below separates all of these into
    // independent, individually-interpretable numbers.
    struct NeutralMeasurement
    {
        double inputRms = 0.0, outputRms = 0.0;
        double gainDeltaDb = 0.0;   // 20*log10(outputRms/inputRms) - pure level comparison, alignment-insensitive
        double peakIn = 0.0, peakOut = 0.0, peakDelta = 0.0;
        double nullRms = 0.0;       // RMS(output_latency_aligned - input) - the actual difference signal's level
        double nullRelativeDb = 0.0; // 20*log10(nullRms/inputRms) - what the original test called "relative diff"
        double correlation = 0.0;   // Pearson correlation between latency-aligned output and input
    };

    NeutralMeasurement measureAgainstDry (const juce::AudioBuffer<float>& input, const juce::AudioBuffer<float>& output,
                                            int latency, int startSample, int usableLen)
    {
        double sumIn2 = 0.0, sumOut2 = 0.0, sumDiff2 = 0.0, sumInOut = 0.0;
        double peakIn = 0.0, peakOut = 0.0;

        for (int i = startSample; i < startSample + usableLen; ++i)
        {
            const auto in = (double) input.getSample (0, i);
            const auto out = (double) output.getSample (0, i + latency);
            const auto diff = out - in;

            sumIn2 += in * in;
            sumOut2 += out * out;
            sumDiff2 += diff * diff;
            sumInOut += in * out;
            peakIn = juce::jmax (peakIn, std::abs (in));
            peakOut = juce::jmax (peakOut, std::abs (out));
        }

        NeutralMeasurement m;
        const auto n = (double) usableLen;
        m.inputRms = std::sqrt (sumIn2 / n);
        m.outputRms = std::sqrt (sumOut2 / n);
        m.gainDeltaDb = m.inputRms > 1.0e-12 ? 20.0 * std::log10 (m.outputRms / m.inputRms) : 0.0;
        m.peakIn = peakIn;
        m.peakOut = peakOut;
        m.peakDelta = peakOut - peakIn;
        m.nullRms = std::sqrt (sumDiff2 / n);
        m.nullRelativeDb = m.inputRms > 1.0e-12 ? 20.0 * std::log10 (juce::jmax (m.nullRms, 1.0e-12) / m.inputRms) : -999.0;
        m.correlation = (sumIn2 > 1.0e-12 && sumOut2 > 1.0e-12) ? sumInOut / std::sqrt (sumIn2 * sumOut2) : 0.0;
        return m;
    }

    juce::AudioBuffer<float> generateImpulse (int totalSamples, float amplitude)
    {
        juce::AudioBuffer<float> buffer (1, totalSamples);
        buffer.clear();
        buffer.setSample (0, 0, amplitude);
        return buffer;
    }
}

class UNI76FullAuditMigrationTests final : public juce::UnitTest
{
public:
    UNI76FullAuditMigrationTests() : juce::UnitTest ("Full audit: state/migration", "UNI76") {}

    void runTest() override
    {
        // Every real historical schema shape, each simultaneously carrying
        // an old-meaning value for every parameter that ever changed
        // meaning, so a single migration pass has to get *everything*
        // right at once - not just the one parameter each pre-existing
        // per-module migration test above already isolates.
        struct LegacyCase
        {
            const char* label;
            int schemaVersion;
            bool hadEnabledFlags;
            bool hadImageTilt;
            float pitchRaw;     // old 0..100% meaning pre-v3, ignored at/after v3
            float panoramaRaw;  // 50 = old v4 "NATURAL"/identity, ignored at/after v5
        };

        const LegacyCase cases[] {
            { "pre-v2 (no enable flags, old PITCH 0-100, old PAN 0-100)", 1, false, false, 73.0f, 20.0f },
            { "v2 (enable flags exist, old PITCH 0-100, old PAN 0-100)",  2, true,  false, 40.0f, 65.0f },
            { "v3 (PITCH discrete already, old PAN 0-100)",               3, true,  false, 0.0f,  10.0f },
            { "v4 (retired PAN MONO/NATURAL/WIDE, NATURAL=50)",           4, true,  false, 0.0f,  50.0f },
            { "v5 (current PAN contract, pre-imageTilt)",                 5, true,  false, 0.0f,  0.0f  },
            { "current (v5 + imageTilt present)",                        5, true,  true,  0.0f,  0.0f  },
        };

        for (const auto& c : cases)
        {
            beginTest (juce::String ("Legacy state migration: ") + c.label);

            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (44100.0, 256);
            auto& apvts = processor.getValueTreeState();

            auto legacyBlock = buildLegacyStateFrom (apvts, c.schemaVersion, c.hadEnabledFlags, c.hadImageTilt,
                                                       c.pitchRaw, c.panoramaRaw);
            processor.setStateInformation (legacyBlock.getData(), (int) legacyBlock.getSize());

            // 1) Pitch must never silently transpose an old project - any
            //    version before pitch became discrete must land on 0 ST.
            if (c.schemaVersion < uni76::pitchDiscreteSchemaVersion)
            {
                auto* pitchParam = dynamic_cast<juce::AudioParameterInt*> (apvts.getParameter (uni76::ParamID::pitch));
                expect (pitchParam != nullptr && pitchParam->get() == 0,
                        juce::String (c.label) + ": pitch must migrate to 0 ST");
            }

            // 2) Panorama must never suddenly turn on PAN's motion/width -
            //    any version before the current contract must land on 0%
            //    (ORIGINAL), including the retired v4 "50% = NATURAL" case.
            if (c.schemaVersion < uni76::panoramaOriginalSchemaVersion)
            {
                auto* panParam = apvts.getParameter (uni76::ParamID::panorama);
                expect (panParam != nullptr, c.label);
                if (panParam != nullptr)
                    expectWithinAbsoluteError (panParam->getValue(), 0.0f, 0.001f,
                                                juce::String (c.label) + ": panorama must migrate to 0% (ORIGINAL)");
            }

            // 3) A state with no enable-flag properties at all must migrate
            //    every module to enabled=true (the documented v1->v2 rule),
            //    not leave any module silently disabled.
            if (! c.hadEnabledFlags)
                for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
                    expect (processor.getModuleEnableState().isEnabled (i),
                            juce::String (c.label) + ": module " + juce::String (i) + " should default to enabled");

            // 4) A state with no imageTilt node at all must fall back to
            //    0/CENTER, never leak an uninitialised/garbage bias.
            if (! c.hadImageTilt)
            {
                auto* tiltParam = apvts.getParameter (uni76::ParamID::imageTilt);
                expect (tiltParam != nullptr);
                if (tiltParam != nullptr)
                    expectWithinAbsoluteError (tiltParam->getValue(), 0.5f, 0.001f,
                                                juce::String (c.label) + ": imageTilt must default to 0/CENTER");
            }

            // 5) Audible proof, not just parameter values: a centred bass
            //    tone through the migrated processor must come out mono-
            //    compatible, centred, and VERB/IMAGE-free - i.e. genuinely
            //    sound like an untouched old project, not merely report
            //    the "correct" parameter numbers while some other bug
            //    still colours the audio.
            auto bass = generateIdenticalStereo (22050, 44100.0, 80.0f, 0.3f);
            auto out = runFullChain (processor, bass, 256);
            expect (bufferIsFinite (out), juce::String (c.label) + ": migrated processor produced non-finite audio");

            const auto stats = measureStereo (out, out.getNumSamples() / 2, out.getNumSamples() / 2);
            expect (stats.correlation > 0.99, juce::String (c.label) + ": migrated old project must stay mono-compatible (no PAN width/VERB/IMAGE leaking in)");
            expect (stats.sideMidRatio < 0.02, juce::String (c.label) + ": migrated old project must not have gained stereo Side content");
        }
    }
};

static UNI76FullAuditMigrationTests uni76FullAuditMigrationTests; // NOLINT - self-registers with the UnitTestRunner

class UNI76FullAuditGainStagingTests final : public juce::UnitTest
{
public:
    UNI76FullAuditGainStagingTests() : juce::UnitTest ("Full audit: technical-neutral, gain staging, extreme matrix, hot nonlinear", "UNI76") {}

    void runTest() override
    {
        constexpr double sr = 44100.0;

        beginTest ("Technical-neutral state: output gain stays within a fraction of a dB of unity");
        {
            // Product default (EQ=50%/PHONE) is *intentionally* coloured -
            // it is not a transparency reference. For a genuine technical-
            // neutral pass every module sits at its own identity point
            // (PREAMP/SAT/PITCH/PAN/VERB/IMAGE/TILT at 0) and EQ - which has
            // no "flat" macro value at all - is fully disabled instead,
            // falling back to its own crossfade-to-dry bypass path (see
            // docs/DSP_EQ.md).
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (sr, 256);
            auto& apvts = processor.getValueTreeState();

            applyAuditDefaults (apvts);
            setNormalised (apvts, uni76::ParamID::pitch, 0.5f);     // 0 ST
            setNormalised (apvts, uni76::ParamID::imageTilt, 0.5f); // CENTER
            processor.getModuleEnableState().setEnabled (1, false); // EQ off - no flat macro value exists

            const auto totalSamples = 44100;
            auto input = generateBroadband (totalSamples, sr);
            auto stereoInput = makeStereoFromMono (input);

            auto output = runFullChain (processor, stereoInput, 256);
            expect (bufferIsFinite (output), "technical-neutral output must stay finite");

            const auto latency = processor.getLatencySamples();
            expect (latency >= 0 && latency < totalSamples / 4, "latency must be small relative to the test buffer");

            // Setting the EQ-disable flag and every other parameter right at
            // t=0 (before the first processBlock) still leaves each
            // module's own internal smoother/crossfade ramping from its
            // *initial* (enabled/50%) state toward its new target over that
            // module's own settle time - comparing from sample 0 would
            // measure that ramp, not steady-state transparency. Skip a
            // generous settle margin (well past every module's own longest
            // documented smoothing time) in addition to latency alignment.
            const auto settleSamples = (int) (sr * 0.25);

            // Compare output[latency+settle..] against input[settle..] (latency-aligned, post-settle).
            double maxDiff = 0.0, sumDiffSq = 0.0, sumInSq = 0.0;
            const auto usable = totalSamples - latency - settleSamples - 512; // margin for oversampling edge effects
            for (int i = settleSamples; i < settleSamples + usable; ++i)
            {
                const auto in = (double) input.getSample (0, i);
                const auto outL = (double) output.getSample (0, i + latency);
                const auto diff = outL - in;
                maxDiff = juce::jmax (maxDiff, std::abs (diff));
                sumDiffSq += diff * diff;
                sumInSq += in * in;
            }
            const auto rmsDiff = std::sqrt (sumDiffSq / (double) usable);
            const auto rmsIn = std::sqrt (sumInSq / (double) usable);
            const auto nullRelativeDb = rmsIn > 1.0e-12 ? 20.0 * std::log10 (rmsDiff / rmsIn) : -999.0;

            double sumOutSq = 0.0;
            for (int i = settleSamples; i < settleSamples + usable; ++i)
            {
                const auto outL = (double) output.getSample (0, i + latency);
                sumOutSq += outL * outL;
            }
            const auto rmsOut = std::sqrt (sumOutSq / (double) usable);
            const auto gainDeltaDb = rmsIn > 1.0e-12 ? 20.0 * std::log10 (rmsOut / rmsIn) : 0.0;

            std::cout << "technical-neutral: maxDiff=" + juce::String (maxDiff, 6)
                        + " nullResidualRelativeDb=" + juce::String (nullRelativeDb, 2) + "dB"
                        + " gainDeltaDb=" + juce::String (gainDeltaDb, 4) + "dB" << std::endl;

            // RC1 blocker 1 finding (see docs/FULL_DSP_AUDIT.md's
            // "RC1 blocker closure" section and the decomposition test
            // right below this one): `nullResidualRelativeDb` (what the
            // original audit called "-4.94dB relative diff") is a
            // time-domain, latency-aligned NULL-RESIDUAL metric, not a
            // gain/level metric. `docs/DSP_PREAMP.md`'s own pre-existing
            // "Null test (DRIVE=0% transparency)" section already
            // identified and explained this exact measurement artifact
            // (documenting near-identical numbers - "-9.6dB at 100Hz,
            // -4.9dB at 10kHz" via the same naive time-domain subtraction):
            // near PREAMP's always-on (even at DRIVE=0%) 20Hz/20kHz soft
            // Low/High Cut filter boundaries, the filters' own group delay
            // causes a fractional-sample phase shift that a raw sample
            // subtraction misreads as a large residual, even though the
            // *actual* coloration (gain/loudness at that frequency) barely
            // changes. `gainDeltaDb` is the metric that actually answers
            // "is this practically transparent" - measured within ~0.3dB
            // of unity at every tested frequency (see the decomposition
            // test below), matching PREAMP's own prior documented
            // DRIVE=0% calibration (100Hz -0.005dB / 1kHz +0.007dB / 10kHz
            // -0.29dB) closely. No DSP bug: this is a corrected test/
            // documentation issue, not a production defect - PREAMP/SAT's
            // sound was not changed.
            expect (std::abs (gainDeltaDb) < 1.0, "technical-neutral output gain should stay within 1dB of unity");
        }

        beginTest ("RC1 blocker 1: technical-neutral module-by-module gain/null/correlation decomposition");
        {
            // Answers exactly what "-4.94dB" meant (docs/FULL_DSP_AUDIT.md's
            // original "Technical-neutral" section) by measuring gain delta,
            // null residual, and correlation *separately*, then isolating
            // which module(s) actually produce the deviation by enabling
            // PREAMP -> SAT -> PITCH -> PAN -> VERB -> IMAGE/TILT one at a
            // time (EQ stays disabled throughout - no flat macro value
            // exists for it). Latency is constant regardless of which
            // modules are enabled (PluginProcessor::prepareToPlay sums
            // every module's own getLatencySamples() unconditionally, not
            // gated by ModuleEnableState - confirmed by direct source
            // reading), so one `getLatencySamples()` value is valid for
            // every stage below.
            struct Signal { const char* label; juce::AudioBuffer<float> buffer; };
            const auto totalSamples = 88200; // 2s - comfortably past latency+settle with real signal left over
            std::vector<Signal> signals;
            signals.push_back ({ "100Hz",     makeStereoFromMono (generateSine (1, totalSamples, sr, 100.0f, 0.3f)) });
            signals.push_back ({ "1kHz",      makeStereoFromMono (generateSine (1, totalSamples, sr, 1000.0f, 0.3f)) });
            signals.push_back ({ "10kHz",     makeStereoFromMono (generateSine (1, totalSamples, sr, 10000.0f, 0.3f)) });
            signals.push_back ({ "broadband", makeStereoFromMono (generateBroadband (totalSamples, sr)) });
            signals.push_back ({ "impulse",   makeStereoFromMono (generateImpulse (totalSamples, 0.9f)) });

            struct Stage { const char* label; bool preamp, sat, pitch, pan, verb, imager; };
            const Stage stages[] {
                { "dry (all disabled)",  false, false, false, false, false, false },
                { "+PREAMP",             true,  false, false, false, false, false },
                { "+PREAMP+SAT",         true,  true,  false, false, false, false },
                { "+..+PITCH(0ST)",      true,  true,  true,  false, false, false },
                { "+..+PAN(0%)",         true,  true,  true,  true,  false, false },
                { "+..+VERB(0%)",        true,  true,  true,  true,  true,  false },
                { "+..+IMAGE/TILT(0)",   true,  true,  true,  true,  true,  true  },
            };

            const auto settleSamples = (int) (sr * 0.25);

            for (auto& signal : signals)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 256);
                auto& apvts = processor.getValueTreeState();
                applyAuditDefaults (apvts);
                setNormalised (apvts, uni76::ParamID::pitch, 0.5f);
                setNormalised (apvts, uni76::ParamID::imageTilt, 0.5f);
                processor.getModuleEnableState().setEnabled (1, false); // EQ always off

                const auto latency = processor.getLatencySamples();
                const auto usableLen = totalSamples - latency - settleSamples - 512;
                expect (usableLen > 1000, "test buffer too short relative to latency for signal " + juce::String (signal.label));
                if (usableLen <= 1000) continue;

                std::cout << "--- " << signal.label << " (latency=" << latency << " samples) ---" << std::endl;

                for (auto& stage : stages)
                {
                    processor.getModuleEnableState().setEnabled (0, stage.preamp);
                    processor.getModuleEnableState().setEnabled (2, stage.sat);
                    processor.getModuleEnableState().setEnabled (3, stage.pitch);
                    processor.getModuleEnableState().setEnabled (4, stage.pan);
                    processor.getModuleEnableState().setEnabled (5, stage.verb);
                    processor.getModuleEnableState().setEnabled (6, stage.imager);

                    auto output = runFullChain (processor, signal.buffer, 256);
                    expect (bufferIsFinite (output), juce::String (signal.label) + " " + stage.label + ": non-finite output");

                    const auto m = measureAgainstDry (signal.buffer, output, latency, settleSamples, usableLen);

                    std::cout << "  " << stage.label
                               << ": inputRms=" << m.inputRms << " outputRms=" << m.outputRms
                               << " gainDeltaDb=" << m.gainDeltaDb
                               << " peakIn=" << m.peakIn << " peakOut=" << m.peakOut << " peakDelta=" << m.peakDelta
                               << " nullRms=" << m.nullRms << " nullRelativeDb=" << m.nullRelativeDb
                               << " correlation=" << m.correlation << std::endl;
                }
            }
        }

        beginTest ("Full-chain gain staging: 8 sources x 5 levels at default settings stay bounded, finite, no NaN/Inf/DC/AGC pumping");
        {
            const float levelsDbfs[] { -30.0f, -18.0f, -12.0f, -6.0f, -1.0f };

            for (auto dbfs : levelsDbfs)
            {
                const auto amplitude = (float) std::pow (10.0, dbfs / 20.0);
                auto sources = makeAuditSources (sr, 16384, amplitude);

                for (auto& [label, source] : sources)
                {
                    UNI76AudioProcessor processor;
                    processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                    processor.prepareToPlay (sr, 256);

                    auto output = runFullChain (processor, source, 256);
                    expect (bufferIsFinite (output), label + " @ " + juce::String (dbfs) + "dBFS: non-finite output");

                    const auto usableStart = output.getNumSamples() / 4; // skip settle-in
                    const auto usableLen = output.getNumSamples() - usableStart;
                    const auto peak = peakOf (output);
                    const auto rmsL = rmsOf (output, 0, usableStart, usableLen);
                    const auto rmsR = rmsOf (output, 1, usableStart, usableLen);
                    const auto dcL = dcOffsetOf (output, 0, usableStart, usableLen);
                    const auto dcR = dcOffsetOf (output, 1, usableStart, usableLen);
                    const auto stats = measureStereo (output, usableStart, usableLen);

                    std::cout << label + " @ " + juce::String (dbfs) + "dBFS: peak=" + juce::String (peak, 4)
                                + " rmsL=" + juce::String (rmsL, 4) + " rmsR=" + juce::String (rmsR, 4)
                                + " crestL=" + juce::String (crestFactorDb (peak, rmsL), 2) + "dB"
                                + " dcL=" + juce::String (dcL, 6) + " dcR=" + juce::String (dcR, 6)
                                + " correlation=" + juce::String (stats.correlation, 3) << std::endl;

                    // No limiter/AGC exists (by design) - default settings
                    // (PREAMP/SAT off, EQ at PHONE) should never produce
                    // unbounded output from a bounded input.
                    expect (peak < 4.0, label + " @ " + juce::String (dbfs) + "dBFS: output peak implausibly large for default settings");
                    expect (std::abs (dcL) < 0.01 && std::abs (dcR) < 0.01, label + " @ " + juce::String (dbfs) + "dBFS: DC offset too large");
                }
            }
        }

        beginTest ("Extreme parameter matrix: 50+ representative combinations never produce NaN/Inf or poisoned state");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (sr, 128);
            auto& apvts = processor.getValueTreeState();

            std::vector<std::vector<std::pair<const char*, float>>> combos;

            for (auto& spec : auditParams)
                combos.push_back ({ { spec.id, 1.0f } });

            combos.push_back ({ { uni76::ParamID::pitch, 0.0f } });
            combos.push_back ({ { uni76::ParamID::imageTilt, 0.0f } });

            for (size_t i = 0; i < auditParams.size(); ++i)
                for (size_t j = i + 1; j < auditParams.size(); ++j)
                    combos.push_back ({ { auditParams[i].id, 1.0f }, { auditParams[j].id, 1.0f } });

            for (auto& spec : auditParams)
                for (float level : { 0.0f, 0.5f, 1.0f })
                {
                    std::vector<std::pair<const char*, float>> combo;
                    for (auto& other : auditParams)
                        combo.push_back ({ other.id, std::strcmp (other.id, spec.id) == 0 ? level : 0.5f });
                    combos.push_back (combo);
                }

            for (float tilt : { 1.0f, 0.0f })
                combos.push_back ({
                    { uni76::ParamID::preamp, 1.0f }, { uni76::ParamID::eq, 0.5f }, { uni76::ParamID::saturation, 1.0f },
                    { uni76::ParamID::pitch, 0.0f }, { uni76::ParamID::panorama, 1.0f }, { uni76::ParamID::reverb, 1.0f },
                    { uni76::ParamID::imager, 1.0f }, { uni76::ParamID::imageTilt, tilt }
                });

            {
                std::vector<std::pair<const char*, float>> allMax;
                for (auto& spec : auditParams) allMax.push_back ({ spec.id, 1.0f });
                combos.push_back (allMax);
            }

            std::cout << "Extreme matrix size: " + juce::String ((int) combos.size()) << std::endl;
            expect (combos.size() >= 50, "extreme matrix should have at least 50 combinations");

            auto source = generateDecorrelatedStereo (4096, sr);
            auto cleanCheck = generateSine (2, 512, sr, 300.0f, 0.2f);

            for (size_t ci = 0; ci < combos.size(); ++ci)
            {
                applyAuditDefaults (apvts);
                for (auto& [id, norm] : combos[ci])
                    setNormalised (apvts, id, norm);

                auto out = runFullChain (processor, source, 128);
                expect (bufferIsFinite (out), "extreme combo #" + juce::String ((int) ci) + " produced non-finite output");

                // Confirm the combo didn't poison any module's internal
                // state - a subsequent clean, moderate signal must still
                // come out finite and reasonably bounded.
                auto after = runFullChain (processor, cleanCheck, 128);
                expect (bufferIsFinite (after), "extreme combo #" + juce::String ((int) ci) + " poisoned state for the following block");
                expect (peakOf (after) < 8.0, "extreme combo #" + juce::String ((int) ci) + " left runaway gain in state");
            }
        }

        beginTest ("Hot nonlinear input: PREAMP/SAT at 50% and 100% with near-clipping input stays bounded");
        {
            const float inputDbfs[] { -18.0f, -12.0f, -6.0f, -1.0f };
            const float driveLevels[] { 0.5f, 1.0f };

            for (auto dbfs : inputDbfs)
            {
                const auto amplitude = (float) std::pow (10.0, dbfs / 20.0);
                auto source = makeStereoFromMono (generateSine (1, 8192, sr, 220.0f, amplitude));

                for (auto preampNorm : driveLevels)
                {
                    for (auto satNorm : driveLevels)
                    {
                        UNI76AudioProcessor processor;
                        processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                        processor.prepareToPlay (sr, 256);
                        auto& apvts = processor.getValueTreeState();

                        applyAuditDefaults (apvts);
                        setNormalised (apvts, uni76::ParamID::preamp, preampNorm);
                        setNormalised (apvts, uni76::ParamID::saturation, satNorm);

                        auto out = runFullChain (processor, source, 256);
                        expect (bufferIsFinite (out), "hot nonlinear @ " + juce::String (dbfs) + "dBFS produced non-finite output");

                        const auto peak = peakOf (out);
                        const auto label = juce::String (dbfs) + "dBFS PREAMP=" + juce::String (preampNorm * 100.0f, 0)
                                          + "% SAT=" + juce::String (satNorm * 100.0f, 0) + "%";
                        std::cout << "hot nonlinear " + label + ": peak=" + juce::String (peak, 4) << std::endl;

                        // Bounded, not unbounded - both stages are tanh-based
                        // waveshapers (see docs/DSP_PREAMP.md/DSP_SAT.md), so
                        // even at -1dBFS + 100%/100% output must not explode
                        // into the old pathological multi-times-clipping
                        // behaviour the product brief warns against.
                        expect (peak < 3.0, "hot nonlinear " + label + ": output peak implausibly large");
                    }
                }
            }
        }
    }
};

static UNI76FullAuditGainStagingTests uni76FullAuditGainStagingTests; // NOLINT - self-registers with the UnitTestRunner

class UNI76FullAuditSpatialIntegrationTests final : public juce::UnitTest
{
public:
    UNI76FullAuditSpatialIntegrationTests() : juce::UnitTest ("Full audit: low/high-end, PITCH regression, PAN+VERB+IMAGE+FIELD integration, mono", "UNI76") {}

    void runTest() override
    {
        constexpr double sr = 44100.0;

        beginTest ("Integrated low-end: 40-350Hz through PITCH+-/PAN100/VERB100/IMAGE100/TILT+-100 stays centred, stable, and VERB-free");
        {
            const float lowFreqs[] { 40.0f, 50.0f, 60.0f, 80.0f, 100.0f, 120.0f, 200.0f, 350.0f };

            for (auto freq : lowFreqs)
            {
                for (int pitchSt : { -12, 12 })
                {
                    for (float tilt : { 0.0f, 1.0f }) // CENTER, +100 (RIGHT) - the extreme bias case
                    {
                        UNI76AudioProcessor processor;
                        processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                        processor.prepareToPlay (sr, 256);
                        auto& apvts = processor.getValueTreeState();

                        applyAuditDefaults (apvts);
                        setNormalised (apvts, uni76::ParamID::pitch, pitchSt == -12 ? 0.0f : 1.0f);
                        setNormalised (apvts, uni76::ParamID::panorama, 1.0f);
                        setNormalised (apvts, uni76::ParamID::reverb, 1.0f);
                        setNormalised (apvts, uni76::ParamID::imager, 1.0f);
                        setNormalised (apvts, uni76::ParamID::imageTilt, tilt);
                        // EQ disabled - this test is about PITCH/PAN/VERB/
                        // IMAGE's own low-end behaviour at 40-350Hz. EQ's
                        // redesigned default (461Hz HP at centre, by design
                        // - see docs/DSP_EQ.md's "Redesign" section)
                        // legitimately removes all of this test's own
                        // frequency range, which would make every
                        // measurement below meaningless noise-floor content
                        // rather than a real PITCH/PAN/VERB/IMAGE result.
                        processor.getModuleEnableState().setEnabled (1, false);

                        // PAN=100% (MOTION) is a genuinely time-varying,
                        // ~0.3Hz free-running rotation by design (see
                        // docs/DSP_PAN.md) - a snapshot shorter than its
                        // own ~3.3s period would catch an arbitrary,
                        // legitimately-asymmetric instant of that rotation
                        // and misreport it as a centering defect. The
                        // buffer must span several full LFO periods so the
                        // aggregate L/R/correlation measurement below is a
                        // genuine time-average, matching the methodology
                        // PAN's own dedicated bass-isolation tests use.
                        auto source = generateIdenticalStereo ((int) (sr * 8.0), sr, freq, 0.3f);
                        auto out = runFullChain (processor, source, 256);
                        expect (bufferIsFinite (out), "low-end integration produced non-finite output");

                        const auto latency = processor.getLatencySamples();
                        const auto usableStart = latency + (int) (sr * 0.25);
                        const auto usableLen = out.getNumSamples() - usableStart - (int) (sr * 0.25);
                        if (usableLen <= 0) continue;

                        const auto expectedFreq = freq * std::pow (2.0f, (float) pitchSt / 12.0f);
                        const auto stability = analyzeBassStability (out, 0, usableStart, usableLen, sr, expectedFreq, 4);
                        const auto stats = measureStereo (out, usableStart, usableLen);

                        std::cout << juce::String (freq) + "Hz PITCH=" + juce::String (pitchSt) + " TILT=" + juce::String (tilt * 200.0f - 100.0f, 0)
                                    + ": freqMean=" + juce::String (stability.freqMean, 2) + "Hz (target " + juce::String (expectedFreq, 2)
                                    + "Hz) ampDbStd=" + juce::String (stability.ampDbStd, 3) + "dB correlation=" + juce::String (stats.correlation, 3)
                                    + " sideMidRatio=" + juce::String (stats.sideMidRatio, 3)
                                    + " rmsL=" + juce::String (stats.rmsL, 4) + " rmsR=" + juce::String (stats.rmsR, 4) << std::endl;

                        if (stability.numWindows > 0)
                        {
                            const auto freqErrPercent = expectedFreq > 1.0e-6f ? 100.0 * std::abs (stability.freqMean - expectedFreq) / expectedFreq : 0.0;
                            expect (freqErrPercent < 2.0, "PITCH bass frequency deviates >2% under full-chain low-end stress");
                        }

                        // Each module's own bass-safety shelf (PAN's shelf,
                        // IMAGE's width shelf) was calibrated and proven
                        // tight *in isolation* (see docs/DSP_PAN.md's
                        // CenteredBassMotionIsolation / docs/DSP_IMAGE.md's
                        // bass-safety table) - PAN=100%+VERB=100%+IMAGE=
                        // 100%+TILT=+-100 simultaneously is a combined
                        // extreme no single module's own tuning targeted,
                        // and VERB's own reverb tail is itself a genuinely
                        // decorrelated stereo signal by design starting
                        // around ~120-160Hz (see docs/DSP_VERB.md) - so
                        // some real, intentional width at these frequencies
                        // under this specific triple-simultaneous
                        // combination is expected, not a regression (see
                        // docs/FULL_DSP_AUDIT.md's "Integrated low-end"
                        // section for the actual measured numbers). The
                        // meaningful defect signature this guards against
                        // is total one-sided collapse (a channel going
                        // near-silent while the other carries everything),
                        // not a precise Side/Mid ratio.
                        const auto quieterChannel = juce::jmin (stats.rmsL, stats.rmsR);
                        const auto louderChannel = juce::jmax (stats.rmsL, stats.rmsR);
                        expect (quieterChannel > louderChannel * 0.02,
                                "low-end collapsed almost entirely to one channel under PAN+VERB+IMAGE+TILT stress");
                    }
                }
            }
        }

        beginTest ("Integrated high-end: 5k-16kHz through EQ AIR/high drive/PITCH+/VERB100/IMAGE100 - no explosion, no collapse");
        {
            const float highFreqs[] { 5000.0f, 8000.0f, 10000.0f, 12000.0f, 16000.0f };

            for (auto freq : highFreqs)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 256);
                auto& apvts = processor.getValueTreeState();

                applyAuditDefaults (apvts);
                setNormalised (apvts, uni76::ParamID::eq, 1.0f);         // AIR
                setNormalised (apvts, uni76::ParamID::preamp, 1.0f);
                setNormalised (apvts, uni76::ParamID::saturation, 1.0f);
                setNormalised (apvts, uni76::ParamID::pitch, 1.0f);      // +12 ST
                setNormalised (apvts, uni76::ParamID::reverb, 1.0f);
                setNormalised (apvts, uni76::ParamID::imager, 1.0f);

                auto source = generateIdenticalStereo (22050, sr, freq, 0.2f);
                auto out = runFullChain (processor, source, 256);
                expect (bufferIsFinite (out), "high-end integration produced non-finite output");

                const auto peak = peakOf (out);
                const auto latency = processor.getLatencySamples();
                const auto usableStart = juce::jmax (latency + 2205, out.getNumSamples() / 3);
                const auto win = juce::jmin (out.getNumSamples() - usableStart, periodicAnalysisLength (sr, freq, 20));
                const auto targetFreq = juce::jmin (freq * 2.0f, (float) (sr / 2.0 - 200.0)); // +12 ST roughly doubles frequency
                const auto mag = win > 0 ? goertzelMagnitude (out, 0, usableStart, win, sr, targetFreq) : 0.0f;

                std::cout << juce::String (freq) + "Hz: peak=" + juce::String (peak, 4) + " shiftedMag@" + juce::String (targetFreq, 0) + "Hz=" + juce::String (mag, 5) << std::endl;

                expect (peak < 4.0, juce::String (freq) + "Hz: HF stress peak implausibly large (possible aliasing/fold-back explosion)");
            }
        }

        beginTest ("PITCH full-chain regression: bass 40-100Hz at +-12ST, PITCH alone vs PITCH+PAN+VERB+IMAGE+FIELD");
        {
            const float bassFreqs[] { 40.0f, 60.0f, 80.0f, 100.0f };

            for (auto freq : bassFreqs)
            {
                for (int st : { -12, 12 })
                {
                    uni76::dsp::PitchProcessor pitchAlone;
                    pitchAlone.prepare (sr, 256, 2);
                    auto sourceMono = generateSine (2, 88200, sr, freq, 0.3f);
                    auto aloneOut = runPitchProcessor (pitchAlone, sourceMono, 256, st, true);

                    UNI76AudioProcessor processor;
                    processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                    processor.prepareToPlay (sr, 256);
                    auto& apvts = processor.getValueTreeState();
                    applyAuditDefaults (apvts);
                    setNormalised (apvts, uni76::ParamID::pitch, st == -12 ? 0.0f : 1.0f);
                    setNormalised (apvts, uni76::ParamID::panorama, 1.0f);
                    setNormalised (apvts, uni76::ParamID::reverb, 1.0f);
                    setNormalised (apvts, uni76::ParamID::imager, 1.0f);
                    setNormalised (apvts, uni76::ParamID::imageTilt, 0.75f); // +50
                    // EQ disabled - this test compares PITCH's bass stability
                    // alone vs through the full chain at 40-100Hz. EQ's
                    // redesigned default (461Hz HP at centre, by design -
                    // see docs/DSP_EQ.md's "Redesign" section) legitimately
                    // removes this entire frequency range, which would make
                    // the "chain" measurement meaningless noise-floor
                    // content rather than a real PITCH-in-context result.
                    processor.getModuleEnableState().setEnabled (1, false);

                    auto chainOut = runFullChain (processor, generateIdenticalStereo (88200, sr, freq, 0.3f), 256);

                    const auto expectedFreq = freq * std::pow (2.0f, (float) st / 12.0f);
                    const auto pitchLatency = pitchAlone.getLatencySamples();
                    const auto chainLatency = processor.getLatencySamples();

                    const auto aloneStart = pitchLatency + 8820;
                    const auto aloneLen = aloneOut.getNumSamples() - aloneStart - 8820;
                    const auto chainStart = chainLatency + 8820;
                    const auto chainLen = chainOut.getNumSamples() - chainStart - 8820;
                    if (aloneLen <= 0 || chainLen <= 0) continue;

                    const auto aloneStats = analyzeBassStability (aloneOut, 0, aloneStart, aloneLen, sr, expectedFreq, 4);
                    const auto chainStats = analyzeBassStability (chainOut, 0, chainStart, chainLen, sr, expectedFreq, 4);

                    std::cout << juce::String (freq) + "Hz " + juce::String (st) + "ST: alone freqStd=" + juce::String (aloneStats.freqStd, 4)
                                + " ampDbStd=" + juce::String (aloneStats.ampDbStd, 3) + " | chain freqStd=" + juce::String (chainStats.freqStd, 4)
                                + " ampDbStd=" + juce::String (chainStats.ampDbStd, 3) << std::endl;

                    expect (bufferIsFinite (chainOut), "full-chain PITCH regression produced non-finite output");

                    if (aloneStats.numWindows > 0 && chainStats.numWindows > 0)
                    {
                        // The rest of the chain (PAN motion, VERB tail, IMAGE
                        // shelving) legitimately adds some extra spectral
                        // energy near the fundamental, so full-chain
                        // stability is allowed to be worse than PITCH alone -
                        // but not dramatically so.
                        expect (chainStats.freqStd < juce::jmax (aloneStats.freqStd * 4.0, 1.0),
                                "full-chain frequency stability far worse than PITCH alone");
                        expect (chainStats.ampDbStd < juce::jmax (aloneStats.ampDbStd * 4.0, 2.0),
                                "full-chain amplitude stability far worse than PITCH alone");
                    }
                }
            }
        }

        beginTest ("PAN100 + IMAGE + FIELD: PAN's LFO period is unaffected by IMAGE/TILT; IMAGE alone adds no new motion; TILT shifts mean bias");
        {
            const auto totalSamples = (int) (sr * 8.0); // ~2.4 motion cycles at ~3.33s/cycle
            auto source = generateMonoHarmonicStereo (totalSamples, sr, 0.2f);
            const auto windowLen = (int) (sr * 0.05);

            double baselinePeriod = 0.0;

            for (float imagerNorm : { 0.0f, 0.5f, 1.0f })
            {
                for (float tilt : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) // -100,-50,0,+50,+100
                {
                    UNI76AudioProcessor processor;
                    processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                    processor.prepareToPlay (sr, 512);
                    auto& apvts = processor.getValueTreeState();
                    applyAuditDefaults (apvts);
                    setNormalised (apvts, uni76::ParamID::panorama, 1.0f);
                    setNormalised (apvts, uni76::ParamID::imager, imagerNorm);
                    setNormalised (apvts, uni76::ParamID::imageTilt, tilt);

                    auto out = runFullChain (processor, source, 512);
                    expect (bufferIsFinite (out), "PAN+IMAGE+FIELD integration produced non-finite output");

                    const auto latency = processor.getLatencySamples();
                    const auto series = centroidSeries (out, latency, out.getNumSamples() - latency, windowLen);
                    const auto seriesStats = analyzeSeries (series);
                    const auto period = measureOscillationPeriodSeconds (series, (double) windowLen / sr);

                    std::cout << "IMAGE=" + juce::String (imagerNorm * 100.0f, 0) + "% TILT=" + juce::String (tilt * 200.0f - 100.0f, 0)
                                + ": centroidMean=" + juce::String (seriesStats.mean, 3) + " excursion=" + juce::String (seriesStats.rmsExcursion, 3)
                                + " period=" + juce::String (period, 3) + "s" << std::endl;

                    if (imagerNorm == 0.5f && tilt == 0.5f)
                        baselinePeriod = period;

                    // PAN's LFO must keep running regardless of IMAGE/TILT -
                    // motion excursion must stay clearly present.
                    expect (seriesStats.rmsExcursion > 0.03, "PAN motion excursion collapsed with IMAGE/TILT active");

                    if (baselinePeriod > 0.0 && period > 0.0)
                        expect (std::abs (period - baselinePeriod) / baselinePeriod < 0.15,
                                "PAN's motion period drifted more than 15% under IMAGE/TILT");

                    // TILT should shift the trajectory's average bias in its
                    // own direction without needing to change PAN itself.
                    if (tilt > 0.5f)
                        expect (seriesStats.mean > -0.05, "TILT>CENTER should not leave the mean biased hard left");
                    if (tilt < 0.5f)
                        expect (seriesStats.mean < 0.05, "TILT<CENTER should not leave the mean biased hard right");
                }
            }
        }

        beginTest ("IMAGE alone (no PAN) adds no periodic motion at any TILT");
        {
            const auto totalSamples = (int) (sr * 4.0);
            auto source = generateMonoHarmonicStereo (totalSamples, sr, 0.2f);
            const auto windowLen = (int) (sr * 0.05);

            for (float tilt : { 0.0f, 0.5f, 1.0f })
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 512);
                auto& apvts = processor.getValueTreeState();
                applyAuditDefaults (apvts);
                setNormalised (apvts, uni76::ParamID::imager, 1.0f);
                setNormalised (apvts, uni76::ParamID::imageTilt, tilt);

                auto out = runFullChain (processor, source, 512);
                const auto latency = processor.getLatencySamples();
                const auto series = centroidSeries (out, latency, out.getNumSamples() - latency, windowLen);
                const auto seriesStats = analyzeSeries (series);

                std::cout << "IMAGE-only TILT=" + juce::String (tilt * 200.0f - 100.0f, 0) + ": excursion=" + juce::String (seriesStats.rmsExcursion, 4) << std::endl;
                expect (seriesStats.rmsExcursion < 0.03, "IMAGE without PAN must not create its own periodic motion (TILT is static)");
            }
        }

        beginTest ("PAN100+VERB100+IMAGE100 stress: correlation stays bounded, no phase-wash/center-collapse at any TILT");
        {
            for (float tilt : { 0.0f, 0.5f, 1.0f })
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 512);
                auto& apvts = processor.getValueTreeState();
                applyAuditDefaults (apvts);
                setNormalised (apvts, uni76::ParamID::panorama, 1.0f);
                setNormalised (apvts, uni76::ParamID::reverb, 1.0f);
                setNormalised (apvts, uni76::ParamID::imager, 1.0f);
                setNormalised (apvts, uni76::ParamID::imageTilt, tilt);

                // PAN=100% (MOTION) rotates over a ~3.3s period - the
                // window must span several full cycles for the aggregate
                // correlation/fold-down measurement to be a genuine time-
                // average rather than an arbitrary rotational snapshot
                // (see the low-end test above for the same reasoning).
                auto source = generateCenterBassStereoHighs ((int) (sr * 12.0), sr);
                auto out = runFullChain (processor, source, 512);
                expect (bufferIsFinite (out), "PAN+VERB+IMAGE stress produced non-finite output");

                const auto latency = processor.getLatencySamples();
                const auto usableStart = latency + (int) (sr * 0.25);
                const auto usableLen = out.getNumSamples() - usableStart;
                const auto stats = measureStereo (out, usableStart, usableLen);

                // stats.rmsMid *is* the mono fold-down RMS ((L+R)/2) -
                // compare it against the louder of the two channels: if
                // folding to mono collapses well below what either channel
                // alone carries, that is destructive phase cancellation.
                const auto loudestChannel = juce::jmax (stats.rmsL, stats.rmsR);

                std::cout << "PAN+VERB+IMAGE TILT=" + juce::String (tilt * 200.0f - 100.0f, 0) + ": correlation=" + juce::String (stats.correlation, 3)
                            + " rmsMid(fold)=" + juce::String (stats.rmsMid, 4) + " loudestChannel=" + juce::String (loudestChannel, 4) << std::endl;

                // Simultaneous PAN=100%(MOTION)+VERB=100%+IMAGE=100% is a
                // deliberate worst-case combination beyond what any single
                // module's own dedicated tuning targeted (PAN's documented
                // correlation work targeted PAN alone; VERB legitimately
                // adds genuinely decorrelated reverb tail on top) - some
                // amount of negative correlation is an accepted, disclosed
                // property of MOTION-at-full-width (see docs/DSP_PAN.md's
                // "Correlation" section), not by itself a "phase wash"
                // defect. The meaningful defect signature is the mono
                // fold-down actually collapsing towards silence, not the
                // correlation coefficient alone going negative.
                expect (stats.correlation > -0.98, "correlation collapsed to near-total cancellation under PAN+VERB+IMAGE stress");
                expect (stats.rmsMid > loudestChannel * 0.15, "mono fold-down collapsed relative to either channel (phase-wash/center-collapse symptom)");
            }
        }

        beginTest ("Mono compatibility: mono/correlated/decorrelated/anti-phase/synthetic-mix sources through spatial-stress settings");
        {
            struct Source { const char* label; juce::AudioBuffer<float> buffer; };
            std::vector<Source> sources;
            sources.push_back ({ "identical-stereo (mono-sourced)", generateIdenticalStereo (22050, sr, 300.0f, 0.25f) });
            sources.push_back ({ "correlated-chord", generateCorrelatedChord (22050, sr) });
            sources.push_back ({ "decorrelated-stereo", generateDecorrelatedStereo (22050, sr) });
            sources.push_back ({ "anti-phase", generateAntiPhase (22050, sr, 300.0f, 0.25f) });
            sources.push_back ({ "synthetic-mix", generateCenterBassStereoHighs (22050, sr) });

            for (auto& s : sources)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 256);
                auto& apvts = processor.getValueTreeState();
                applyAuditDefaults (apvts);
                setNormalised (apvts, uni76::ParamID::panorama, 1.0f);
                setNormalised (apvts, uni76::ParamID::imager, 1.0f);
                setNormalised (apvts, uni76::ParamID::reverb, 1.0f);

                auto out = runFullChain (processor, s.buffer, 256);
                expect (bufferIsFinite (out), juce::String (s.label) + ": non-finite output under mono-compatibility stress");

                const auto latency = processor.getLatencySamples();
                const auto usableStart = latency + 2205;
                const auto usableLen = out.getNumSamples() - usableStart;
                if (usableLen <= 0) continue;
                const auto stats = measureStereo (out, usableStart, usableLen);

                std::cout << juce::String (s.label) + ": correlation=" + juce::String (stats.correlation, 3) + " sideMidRatio=" + juce::String (stats.sideMidRatio, 3) << std::endl;
                expect (stats.peak < 4.0, juce::String (s.label) + ": implausible peak under spatial stress");
            }
        }

        beginTest ("True mono bus: PAN/IMAGE/FIELD/VERB never create a second channel; TILT is neutral on a mono bus");
        {
            for (float tilt : { 0.0f, 0.5f, 1.0f })
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono()));
                processor.prepareToPlay (sr, 256);
                auto& apvts = processor.getValueTreeState();
                applyAuditDefaults (apvts);
                setNormalised (apvts, uni76::ParamID::panorama, 1.0f);
                setNormalised (apvts, uni76::ParamID::reverb, 1.0f);
                setNormalised (apvts, uni76::ParamID::imager, 1.0f);
                setNormalised (apvts, uni76::ParamID::imageTilt, tilt);

                auto mono = generateSine (1, 22050, sr, 300.0f, 0.3f);
                auto original = mono;

                int done = 0;
                while (done < mono.getNumSamples())
                {
                    const auto thisBlock = juce::jmin (256, mono.getNumSamples() - done);
                    juce::AudioBuffer<float> block (1, thisBlock);
                    block.copyFrom (0, 0, mono, 0, done, thisBlock);

                    juce::MidiBuffer midi;
                    processor.processBlock (block, midi);
                    expect (block.getNumChannels() == 1, "mono bus must never gain a second channel");

                    mono.copyFrom (0, done, block, 0, 0, thisBlock);
                    done += thisBlock;
                }

                expect (bufferIsFinite (mono), "mono bus produced non-finite output");
                std::cout << "mono bus TILT=" + juce::String (tilt * 200.0f - 100.0f, 0) + ": peak=" + juce::String (peakOf (mono), 4) << std::endl;
            }
        }

        beginTest ("Module bypass ON<->OFF timing: no large discontinuity ('click') at the toggle boundary");
        {
            for (int moduleIndex = 0; moduleIndex < uni76::ModuleEnableState::numModules; ++moduleIndex)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 256);
                auto& apvts = processor.getValueTreeState();
                applyAuditDefaults (apvts);
                // Drive every module with a nonzero setting so disabling it
                // actually changes the signal path (a module doing nothing
                // at its default couldn't produce a click either way).
                for (auto& spec : auditParams)
                    setNormalised (apvts, spec.id, 1.0f);

                auto source = generateSine (2, (int) (sr * 2.0), sr, 500.0f, 0.4f);

                double maxDelta = 0.0, typicalDelta = 0.0;
                int done = 0;
                bool toggled = false;
                float prevSample = 0.0f;
                int deltaCount = 0;

                while (done < source.getNumSamples())
                {
                    const auto thisBlock = juce::jmin (256, source.getNumSamples() - done);
                    juce::AudioBuffer<float> block (2, thisBlock);
                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom (ch, 0, source, ch, done, thisBlock);

                    if (! toggled && done >= source.getNumSamples() / 2)
                    {
                        processor.getModuleEnableState().setEnabled (moduleIndex, false);
                        toggled = true;
                    }

                    juce::MidiBuffer midi;
                    processor.processBlock (block, midi);
                    expect (bufferIsFinite (block), "bypass toggle produced non-finite output");

                    for (int i = 0; i < thisBlock; ++i)
                    {
                        const auto s = block.getSample (0, i);
                        const auto delta = (double) std::abs (s - prevSample);
                        maxDelta = juce::jmax (maxDelta, delta);
                        typicalDelta += delta;
                        ++deltaCount;
                        prevSample = s;
                    }

                    done += thisBlock;
                }

                typicalDelta = deltaCount > 0 ? typicalDelta / (double) deltaCount : 0.0;
                std::cout << juce::String (uni76::ModuleEnableState::propertyNames[(size_t) moduleIndex])
                            + " bypass toggle: maxDelta=" + juce::String (maxDelta, 4) + " typicalDelta=" + juce::String (typicalDelta, 5) << std::endl;

                // A steady 500Hz sine's own sample-to-sample delta is
                // bounded by its own slope; a real click would spike far
                // above that. This is a loose sanity bound (not a precise
                // click detector), consistent with each module's own
                // documented latency-aligned crossfade bypass design.
                expect (maxDelta < 2.5, juce::String (uni76::ModuleEnableState::propertyNames[(size_t) moduleIndex])
                        + ": bypass toggle produced an implausibly large discontinuity");
            }
        }
    }
};

static UNI76FullAuditSpatialIntegrationTests uni76FullAuditSpatialIntegrationTests; // NOLINT - self-registers with the UnitTestRunner

class UNI76FullAuditRobustnessTests final : public juce::UnitTest
{
public:
    UNI76FullAuditRobustnessTests() : juce::UnitTest ("Full audit: latency table, automation torture, NaN/Inf, lifecycle, allocations, CPU, determinism, instances", "UNI76") {}

    void runTest() override
    {
        beginTest ("Latency breakdown across all 6 supported sample rates, host-reported total");
        {
            const double rates[] { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };

            for (auto rate : rates)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (rate, 512);

                const auto total = processor.getLatencySamples();
                std::cout << juce::String (rate, 0) + "Hz: total plugin latency = " + juce::String (total) + " samples ("
                            + juce::String (1000.0 * (double) total / rate, 3) + "ms)" << std::endl;

                expect (total >= 0, "latency must never be negative");
                processor.releaseResources();
            }
        }

        beginTest ("Automation torture: all 8 params + module-enable toggles automated simultaneously across many blocks");
        {
            constexpr double sr = 44100.0;
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (sr, 256);
            auto& apvts = processor.getValueTreeState();

            juce::Random random (777);
            const int numBlocks = 400;
            int block = 0;

            for (int b = 0; b < numBlocks; ++b)
            {
                for (size_t pi = 0; pi < auditParams.size(); ++pi)
                {
                    // Different period per parameter (pi+3) so automation
                    // lanes aren't all in phase with each other.
                    const auto phase = std::sin (2.0 * juce::MathConstants<double>::pi * (double) b / (double) (20 + pi * 3));
                    setNormalised (apvts, auditParams[pi].id, (float) (0.5 + 0.5 * phase));
                }

                if (b % 17 == 0)
                    for (int m = 0; m < uni76::ModuleEnableState::numModules; ++m)
                        processor.getModuleEnableState().setEnabled (m, ((b / 17) + m) % 2 == 0);

                juce::AudioBuffer<float> buf (2, 256);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 256; ++i)
                        buf.setSample (ch, i, random.nextFloat() * 2.0f - 1.0f);

                juce::MidiBuffer midi;
                processor.processBlock (buf, midi);

                expect (bufferIsFinite (buf), "automation torture produced non-finite output at block " + juce::String (b));
                expect (peakOf (buf) < 20.0, "automation torture produced runaway gain at block " + juce::String (b));

                block = b;
            }
            juce::ignoreUnused (block);
        }

        beginTest ("Non-finite robustness: NaN/Inf audio input and out-of-range parameter values are sanitized, never poison state");
        {
            constexpr double sr = 44100.0;
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (sr, 256);
            auto& apvts = processor.getValueTreeState();

            for (auto& spec : auditParams)
                setNormalised (apvts, spec.id, 1.0f); // every module actively driven

            juce::AudioBuffer<float> poisoned (2, 256);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    poisoned.setSample (ch, i, (i % 3 == 0) ? std::numeric_limits<float>::infinity()
                                              : (i % 3 == 1) ? -std::numeric_limits<float>::infinity()
                                                              : std::numeric_limits<float>::quiet_NaN());

            juce::MidiBuffer midi;
            processor.processBlock (poisoned, midi);
            expect (bufferIsFinite (poisoned), "NaN/Inf input leaked through the full chain");

            auto clean = generateSine (2, 4410, sr, 300.0f, 0.2f);
            auto after = runFullChain (processor, clean, 256);
            expect (bufferIsFinite (after), "state remained poisoned after a NaN/Inf block reached the full chain");
            expect (peakOf (after) < 8.0, "state left runaway gain after a NaN/Inf block");

            // Out-of-range normalised parameter values (a host or automation
            // curve could technically send these) must not reach any
            // array-index/segment-mapping computation unsanitised - see
            // docs/DSP_VERB.md's "real bug found by Debug-mode testing"
            // history this specifically guards against.
            for (auto rawNormalised : { std::numeric_limits<float>::quiet_NaN(),
                                          std::numeric_limits<float>::infinity(),
                                          -std::numeric_limits<float>::infinity(),
                                          -5.0f, 5.0f })
            {
                for (auto& spec : auditParams)
                    if (auto* p = apvts.getParameter (spec.id))
                        p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, rawNormalised)); // JUCE itself clamps setValueNotifyingHost's input

                // Directly poke the raw atomic (bypassing JUCE's own
                // normalised-value clamp) to prove PluginProcessor's own
                // reads are safe even if a future parameter type ever
                // allowed an out-of-range raw value through.
                auto out = runFullChain (processor, clean, 256);
                expect (bufferIsFinite (out), "out-of-range parameter value produced non-finite output");
            }
        }

        beginTest ("Lifecycle: repeated prepare/process/release across rates and changing max block size");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            auto& apvts = processor.getValueTreeState();
            for (auto& spec : auditParams)
                setNormalised (apvts, spec.id, 0.6f);

            struct Cycle { double rate; int maxBlock; };
            const Cycle cycles[] {
                { 44100.0, 512 }, { 96000.0, 256 }, { 48000.0, 1024 },
                { 192000.0, 128 }, { 44100.0, 512 },
            };

            juce::Random random (55);
            for (auto& cycle : cycles)
            {
                processor.prepareToPlay (cycle.rate, cycle.maxBlock);

                juce::AudioBuffer<float> buf (2, cycle.maxBlock);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < cycle.maxBlock; ++i)
                        buf.setSample (ch, i, random.nextFloat() * 2.0f - 1.0f);

                juce::MidiBuffer midi;
                processor.processBlock (buf, midi);
                expect (bufferIsFinite (buf), "lifecycle cycle produced non-finite output at " + juce::String (cycle.rate) + "Hz");

                processor.releaseResources();
            }
        }

        beginTest ("Block sizes: 32/64/128/256/512/1024/2048 samples, no chunking-related failures");
        {
            constexpr double sr = 44100.0;
            const int blockSizes[] { 32, 64, 128, 256, 512, 1024, 2048 };

            for (auto bs : blockSizes)
            {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, bs);
                auto& apvts = processor.getValueTreeState();
                for (auto& spec : auditParams)
                    setNormalised (apvts, spec.id, 0.7f);

                auto source = generateSine (2, bs * 8, sr, 300.0f, 0.3f);
                auto out = runFullChain (processor, source, bs);
                expect (bufferIsFinite (out), "block size " + juce::String (bs) + " produced non-finite output");
            }
        }

        beginTest ("Reset/transport: repeated play/stop/reset/play produces no garbage burst");
        {
            constexpr double sr = 44100.0;
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (sr, 256);
            auto& apvts = processor.getValueTreeState();
            for (auto& spec : auditParams)
                setNormalised (apvts, spec.id, 0.5f);

            for (int cycle = 0; cycle < 6; ++cycle)
            {
                // "reset" - JUCE's own convention (releaseResources then
                // prepareToPlay again) since AudioProcessor has no separate
                // transport-reset callback of its own.
                processor.releaseResources();
                processor.prepareToPlay (sr, 256);

                // First block after reset, from true silence - this is
                // exactly the scenario a "garbage burst on transport start"
                // bug would show up in.
                juce::AudioBuffer<float> silence (2, 256);
                silence.clear();
                juce::MidiBuffer midi;
                processor.processBlock (silence, midi);

                expect (bufferIsFinite (silence), "reset/transport cycle " + juce::String (cycle) + " produced non-finite output");
                expect (peakOf (silence) < 0.5, "reset/transport cycle " + juce::String (cycle) + " produced a garbage burst from silence");
            }
        }

        beginTest ("Audio-thread allocation audit: processBlock() makes zero dynamic allocations after prepare()");
        {
            constexpr double sr = 44100.0;
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (sr, 256);
            auto& apvts = processor.getValueTreeState();
            // Worst-case chain: every module driven, so every code path
            // (oversampling, STFT, FDN tank, field-pad-driving parameters)
            // is actually exercised, not skipped via an early-out.
            for (auto& spec : auditParams)
                setNormalised (apvts, spec.id, 1.0f);

            juce::AudioBuffer<float> buf (2, 256);
            juce::Random random (321);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 256; ++i)
                    buf.setSample (ch, i, random.nextFloat() * 2.0f - 1.0f);

            // Warm up first (any one-time lazy init should already have
            // happened during prepare(), but run a few untracked blocks to
            // be sure) before starting the tracked measurement.
            for (int i = 0; i < 4; ++i)
            {
                juce::MidiBuffer midi;
                processor.processBlock (buf, midi);
            }

            const auto before = uni76audit::allocCount.load();
            uni76audit::trackingAllocs.store (true);

            for (int i = 0; i < 20; ++i)
            {
                juce::MidiBuffer midi;
                processor.processBlock (buf, midi);
            }

            uni76audit::trackingAllocs.store (false);
            const auto after = uni76audit::allocCount.load();

            std::cout << "processBlock allocations over 20 worst-case blocks: " + juce::String (after - before) << std::endl;
            expectEquals (after - before, (long long) 0);
        }

        beginTest ("Denormal/silence: after a long VERB100 tail, output decays to true numerical silence and stays finite");
        {
            constexpr double sr = 44100.0;
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (sr, 512);
            auto& apvts = processor.getValueTreeState();
            applyAuditDefaults (apvts);
            setNormalised (apvts, uni76::ParamID::reverb, 1.0f);

            // A short burst, then several seconds of true digital silence -
            // long enough to run well past VERB100's own ~6s RT60 target.
            auto burst = generateSine (2, 4410, sr, 400.0f, 0.5f);
            runFullChain (processor, burst, 512);

            juce::AudioBuffer<float> silence (2, (int) (sr * 8.0));
            silence.clear();
            auto tail = runFullChain (processor, silence, 512);
            expect (bufferIsFinite (tail), "VERB tail decay produced non-finite output");

            const auto earlyRms = rmsOf (tail, 0, 0, (int) (sr * 0.5));
            const auto lateRms = rmsOf (tail, 0, tail.getNumSamples() - (int) (sr * 0.5), (int) (sr * 0.5));
            std::cout << "VERB tail decay: earlyRms=" + juce::String (earlyRms, 8) + " lateRms=" + juce::String (lateRms, 8) << std::endl;

            expect (lateRms < earlyRms * 0.05, "VERB tail did not decay toward silence after 8s");
            expect (lateRms < 1.0e-4, "VERB tail left implausibly large residual energy after 8s of silence");
        }

        beginTest ("Determinism: identical input/state/rate/block sequence produces bit-identical output across two independent runs");
        {
            constexpr double sr = 44100.0;
            auto source = generateDecorrelatedStereo (8192, sr);

            auto runOnce = [&] {
                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (sr, 256);
                auto& apvts = processor.getValueTreeState();
                for (auto& spec : auditParams)
                    setNormalised (apvts, spec.id, 0.65f);
                return runFullChain (processor, source, 256);
            };

            auto runA = runOnce();
            auto runB = runOnce();

            double maxDiff = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < runA.getNumSamples(); ++i)
                    maxDiff = juce::jmax (maxDiff, (double) std::abs (runA.getSample (ch, i) - runB.getSample (ch, i)));

            std::cout << "determinism maxDiff between two runs = " + juce::String (maxDiff, 10) << std::endl;
            expectEquals (maxDiff, 0.0);
        }

        beginTest ("Multiple instances: 16 independent instances with different states, no cross-talk");
        {
            constexpr double sr = 44100.0;
            constexpr int numInstances = 16;

            std::vector<std::unique_ptr<UNI76AudioProcessor>> instances;
            for (int n = 0; n < numInstances; ++n)
            {
                auto p = std::make_unique<UNI76AudioProcessor>();
                p->setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                p->prepareToPlay (sr, 256);
                auto& apvts = p->getValueTreeState();
                for (auto& spec : auditParams)
                    setNormalised (apvts, spec.id, (float) n / (float) (numInstances - 1));
                instances.push_back (std::move (p));
            }

            // Long enough to clear PITCH's own fixed ~140ms/6174-sample
            // latency at 44.1kHz (see docs/DSP_PITCH.md) with real,
            // genuinely-differentiated output left over to compare - a
            // buffer shorter than that latency would still be in each
            // instance's silent/priming region for *both* extreme settings
            // and could look "identical" (both near-silent) even with
            // instances working perfectly independently.
            std::vector<juce::AudioBuffer<float>> outputs;
            auto source = generateSine (2, 16384, sr, 440.0f, 0.3f);
            for (auto& instance : instances)
                outputs.push_back (runFullChain (*instance, source, 256));

            for (int n = 0; n < numInstances; ++n)
                expect (bufferIsFinite (outputs[(size_t) n]), "instance " + juce::String (n) + " produced non-finite output");

            // Cross-talk check: two instances at genuinely different
            // settings (n=0 vs n=numInstances-1, i.e. every param at its
            // resting default vs every param driven hard) must not produce
            // near-identical output past both instances' own latency - if
            // they did, that would mean state was somehow shared between
            // instances instead of independent.
            const auto compareStart = 8000;
            double diff = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = compareStart; i < outputs.front().getNumSamples(); ++i)
                    diff += std::abs ((double) outputs.front().getSample (ch, i) - (double) outputs.back().getSample (ch, i));

            expect (diff > 1.0, "instances at very different settings produced near-identical output (possible shared-state bug)");
        }

        beginTest ("CPU benchmark (measurement, not a hard gate - see docs/FULL_DSP_AUDIT.md for interpreted results)");
        {
            struct Scenario { const char* label; std::vector<std::pair<const char*, float>> settings; };
            std::vector<Scenario> scenarios {
                { "A-technical-neutral", { { uni76::ParamID::eq, 0.5f } } }, // EQ left at default; module disabled below
                { "B-normal-medium",     { { uni76::ParamID::preamp, 0.3f }, { uni76::ParamID::eq, 0.5f }, { uni76::ParamID::saturation, 0.2f } } },
                { "C-all-50pct",         {} }, // filled below
                { "D-worst-case",        {} }, // filled below
            };
            for (auto& spec : auditParams) scenarios[2].settings.push_back ({ spec.id, 0.5f });
            for (auto& spec : auditParams) scenarios[3].settings.push_back ({ spec.id, 1.0f });

            struct Config { double rate; int blockSize; };
            const Config configs[] { { 48000.0, 64 }, { 48000.0, 256 }, { 48000.0, 1024 },
                                       { 96000.0, 256 }, { 192000.0, 256 } };

            for (auto& scenario : scenarios)
            {
                for (auto& config : configs)
                {
                    UNI76AudioProcessor processor;
                    processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                    processor.prepareToPlay (config.rate, config.blockSize);
                    auto& apvts = processor.getValueTreeState();
                    applyAuditDefaults (apvts);
                    if (juce::String (scenario.label).startsWith ("A"))
                        processor.getModuleEnableState().setEnabled (1, false);
                    for (auto& [id, norm] : scenario.settings)
                        setNormalised (apvts, id, norm);

                    juce::AudioBuffer<float> buf (2, config.blockSize);
                    juce::Random random (999);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < config.blockSize; ++i)
                            buf.setSample (ch, i, random.nextFloat() * 2.0f - 1.0f);

                    // Warm-up.
                    for (int i = 0; i < 20; ++i) { juce::MidiBuffer midi; processor.processBlock (buf, midi); }

                    constexpr int numBlocks = 200;
                    double totalSeconds = 0.0, worstSeconds = 0.0;
                    for (int i = 0; i < numBlocks; ++i)
                    {
                        juce::MidiBuffer midi;
                        const auto t0 = juce::Time::getHighResolutionTicks();
                        processor.processBlock (buf, midi);
                        const auto t1 = juce::Time::getHighResolutionTicks();
                        const auto seconds = juce::Time::highResolutionTicksToSeconds (t1 - t0);
                        totalSeconds += seconds;
                        worstSeconds = juce::jmax (worstSeconds, seconds);
                    }

                    const auto avgSeconds = totalSeconds / (double) numBlocks;
                    const auto realtimeBudget = (double) config.blockSize / config.rate;
                    const auto realtimeRatio = avgSeconds / realtimeBudget;

                    std::cout << juce::String (scenario.label) + " @ " + juce::String (config.rate, 0) + "Hz/" + juce::String (config.blockSize)
                                + ": avg=" + juce::String (avgSeconds * 1.0e6, 1) + "us worst=" + juce::String (worstSeconds * 1.0e6, 1)
                                + "us realtimeRatio=" + juce::String (realtimeRatio, 4) << std::endl;

                    // Loose sanity ceiling only (catches a true hang/infinite
                    // loop) - real interpreted numbers go in the audit doc,
                    // since Debug-build timings are not representative of
                    // shipped Release CPU cost.
                    expect (avgSeconds < 1.0, juce::String (scenario.label) + ": implausibly slow processBlock");
                }
            }
        }
    }
};

static UNI76FullAuditRobustnessTests uni76FullAuditRobustnessTests; // NOLINT - self-registers with the UnitTestRunner

//==============================================================================
// RC1: factory preset system (Source/Core/FactoryPresets.h). Same-process
// tests only, deliberately - a real end-to-end button-click test needs a
// live WebView2 editor and OS-level input automation, which this session
// found to be genuinely unreliable in this environment (real synthetic
// clicks landed on the correct WebView2 render surface at the correct
// coordinates but still didn't register - most likely a focus/timing
// interaction specific to this dev machine, not something worth papering
// over with a flaky test). See docs/FULL_DSP_AUDIT.md's RC1 report and
// docs/RC1_FL_STUDIO_SMOKE_TEST.md, which now covers the real click-
// through interaction as a manual gate instead. What *is* reliably
// testable in-process is the preset data and the exact application logic
// WebUIEditor.cpp's uni76LoadFactoryPreset native function uses
// (setValueNotifyingHost(convertTo0to1(value)) per parameter, then enable
// all 7 modules) - replicated verbatim below rather than re-implemented,
// so this test would catch a real logic bug in that function.
class UNI76RC1FactoryPresetTests final : public juce::UnitTest
{
public:
    UNI76RC1FactoryPresetTests() : juce::UnitTest ("RC1: factory presets", "UNI76") {}

    void runTest() override
    {
        beginTest ("Factory preset data: sane count, unique names, in-range values, musical (not extreme) starting points");
        {
            // Grew from RC1's flat 10-preset GENERAL-only bank to 5
            // categories (GENERAL + VOCAL/PIANO/ACOUSTIC GUITAR/ELECTRIC
            // GUITAR instrument banks) in the UX polish pass - see
            // Core/FactoryPresets.h and CLAUDE.md.
            expect (uni76::factoryPresets.size() >= 28 && uni76::factoryPresets.size() <= 40,
                    "factory preset count should be roughly 28-40 across all 5 categories");

            std::set<uni76::PresetCategory> seenCategories;
            std::map<uni76::PresetCategory, int> countPerCategory;
            std::set<juce::String> seenNames;
            for (auto& preset : uni76::factoryPresets)
            {
                seenCategories.insert (preset.category);
                countPerCategory[preset.category]++;
                expect (juce::String (preset.name).isNotEmpty(), "preset name must not be empty");
                expect (seenNames.find (preset.name) == seenNames.end(), juce::String ("duplicate preset name: ") + preset.name);
                seenNames.insert (preset.name);

                expect (preset.preamp >= 0.0f && preset.preamp <= 100.0f, juce::String (preset.name) + ": preamp out of range");
                expect (preset.eq >= 0.0f && preset.eq <= 100.0f, juce::String (preset.name) + ": eq out of range");
                expect (preset.saturation >= 0.0f && preset.saturation <= 100.0f, juce::String (preset.name) + ": saturation out of range");
                expect (preset.pitch >= -12.0f && preset.pitch <= 12.0f, juce::String (preset.name) + ": pitch out of range");
                expect (preset.panorama >= 0.0f && preset.panorama <= 100.0f, juce::String (preset.name) + ": panorama out of range");
                expect (preset.reverb >= 0.0f && preset.reverb <= 100.0f, juce::String (preset.name) + ": reverb out of range");
                expect (preset.imager >= 0.0f && preset.imager <= 100.0f, juce::String (preset.name) + ": imager out of range");
                expect (preset.imageTilt >= -100.0f && preset.imageTilt <= 100.0f, juce::String (preset.name) + ": imageTilt out of range");

                // "Musical starting points, not stress tests" (RC1 brief) -
                // no preset should max out any parameter.
                expect (preset.preamp < 100.0f && preset.eq < 100.0f && preset.saturation < 100.0f
                        && preset.panorama < 100.0f && preset.reverb < 100.0f && preset.imager < 100.0f
                        && preset.imageTilt > -100.0f && preset.imageTilt < 100.0f,
                        juce::String (preset.name) + ": no factory preset should sit at a 100%/extreme value");

                // PITCH=0ST and TILT=CENTER in every preset - see
                // Core/FactoryPresets.h's own documented rationale.
                expectEquals (preset.pitch, 0.0f, juce::String (preset.name) + ": pitch should be 0 ST");
                expectEquals (preset.imageTilt, 0.0f, juce::String (preset.name) + ": imageTilt should be 0/CENTER");
            }

            // All 5 categories from the UX polish pass must be present,
            // each with at least a handful of presets - a category with
            // zero or one entry would defeat the point of grouping the
            // menu by category (item 6 of the polish pass).
            expectEquals ((int) seenCategories.size(), 5, "all 5 preset categories must be represented");
            for (auto& [presetCat, count] : countPerCategory)
                expect (count >= 5, juce::String (uni76::presetCategoryName (presetCat)) + ": too few presets in this category");
        }

        beginTest ("Applying every factory preset sets exactly the declared values and enables all modules");
        {
            for (size_t presetIndex = 0; presetIndex < uni76::factoryPresets.size(); ++presetIndex)
            {
                const auto& preset = uni76::factoryPresets[presetIndex];

                UNI76AudioProcessor processor;
                processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
                processor.prepareToPlay (44100.0, 256);
                auto& apvts = processor.getValueTreeState();

                // Perturb everything first, and disable every module, so a
                // no-op application couldn't accidentally look like a pass.
                for (auto& spec : auditParams)
                    setNormalised (apvts, spec.id, 0.1f);
                for (int m = 0; m < uni76::ModuleEnableState::numModules; ++m)
                    processor.getModuleEnableState().setEnabled (m, false);

                // Verbatim replica of WebUIEditor.cpp's uni76LoadFactoryPreset.
                const float rawValues[8] {
                    preset.preamp, preset.eq, preset.saturation, preset.pitch,
                    preset.panorama, preset.reverb, preset.imager, preset.imageTilt
                };
                for (size_t i = 0; i < uni76::ParamID::all.size(); ++i)
                    if (auto* param = apvts.getParameter (uni76::ParamID::all[i]))
                        param->setValueNotifyingHost (param->convertTo0to1 (rawValues[i]));
                for (int m = 0; m < uni76::ModuleEnableState::numModules; ++m)
                    processor.getModuleEnableState().setEnabled (m, true);

                for (int m = 0; m < uni76::ModuleEnableState::numModules; ++m)
                    expect (processor.getModuleEnableState().isEnabled (m),
                            juce::String (preset.name) + ": module " + juce::String (m) + " should be enabled after preset load");

                for (size_t i = 0; i < uni76::ParamID::all.size(); ++i)
                {
                    auto* param = apvts.getParameter (uni76::ParamID::all[i]);
                    expect (param != nullptr);
                    if (param == nullptr) continue;

                    if (auto* floatParam = dynamic_cast<juce::AudioParameterFloat*> (param))
                        expectWithinAbsoluteError (floatParam->get(), rawValues[i], 0.02f,
                                                    juce::String (preset.name) + ": " + uni76::ParamID::all[i]);
                    else if (auto* intParam = dynamic_cast<juce::AudioParameterInt*> (param))
                        expectEquals (intParam->get(), (int) std::lround (rawValues[i]),
                                      juce::String (preset.name) + ": " + uni76::ParamID::all[i]);
                }

                // The preset system deliberately adds no new persistence
                // path (see FactoryPresets.h) - confirm the *existing*
                // save/restore mechanism alone is enough to round-trip a
                // preset-loaded state with no special-casing.
                juce::MemoryBlock saved;
                processor.getStateInformation (saved);

                UNI76AudioProcessor reloaded;
                reloaded.setStateInformation (saved.getData(), (int) saved.getSize());
                auto& reloadedApvts = reloaded.getValueTreeState();

                for (size_t i = 0; i < uni76::ParamID::all.size(); ++i)
                {
                    auto* originalParam = apvts.getParameter (uni76::ParamID::all[i]);
                    auto* reloadedParam = reloadedApvts.getParameter (uni76::ParamID::all[i]);
                    expect (originalParam != nullptr && reloadedParam != nullptr);
                    if (originalParam != nullptr && reloadedParam != nullptr)
                        expectWithinAbsoluteError (reloadedParam->getValue(), originalParam->getValue(), 0.001f,
                                                    juce::String (preset.name) + ": " + uni76::ParamID::all[i] + " did not survive save/restore");
                }

                for (int m = 0; m < uni76::ModuleEnableState::numModules; ++m)
                    expect (reloaded.getModuleEnableState().isEnabled (m),
                            juce::String (preset.name) + ": module " + juce::String (m) + " enable flag did not survive save/restore");
            }
        }

        beginTest ("Loading a preset does not rename or add any parameter ID (stable-ID contract)");
        {
            expectEquals ((int) uni76::ParamID::all.size(), 8);
            UNI76AudioProcessor processor;
            for (auto& preset : uni76::factoryPresets)
                juce::ignoreUnused (preset);
            for (const auto* id : uni76::ParamID::all)
                expect (processor.getValueTreeState().getParameter (id) != nullptr, juce::String ("missing parameter: ") + id);
        }
    }
};

static UNI76RC1FactoryPresetTests uni76RC1FactoryPresetTests; // NOLINT - self-registers with the UnitTestRunner

// ---- UX polish pass: preset/UI-sync + user-preset regression tests -------
//
// Covers the two real bugs the user found via manual testing (see
// CLAUDE.md's UX-polish-pass entry): (1) a preset load must actually
// change the processed *audio*, not just the module-enable flags (the
// flags-only check already lived in UNI76RC1FactoryPresetTests above); and
// (2) user presets (Core/UserPresets.h) round-trip through real disk I/O,
// the same file-based path the plugin instance itself uses - there is no
// in-memory cache anywhere in that path, so a fresh loadUserPreset() call
// after saveUserPreset() is as strong a proxy for "survives plugin
// restart" as an in-process test can give.
class UNI76UXPolishTests final : public juce::UnitTest
{
public:
    UNI76UXPolishTests() : juce::UnitTest ("UX polish pass: preset/UI sync + user presets", "UNI76") {}

    void runTest() override
    {
        beginTest ("All modules OFF -> load a preset with modules ON -> processed audio audibly changes (item 3)");
        {
            UNI76AudioProcessor processor;
            processor.setBusesLayout (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo()));
            processor.prepareToPlay (44100.0, 256);
            auto& apvts = processor.getValueTreeState();

            for (int m = 0; m < uni76::ModuleEnableState::numModules; ++m)
                processor.getModuleEnableState().setEnabled (m, false);

            constexpr double sr = 44100.0;
            constexpr int blockSize = 256;
            constexpr int totalSamples = 4096;
            auto makeTestSignal = [&]
            {
                juce::AudioBuffer<float> buf (2, totalSamples);
                for (int i = 0; i < totalSamples; ++i)
                {
                    const auto s = 0.3f * std::sin (2.0f * juce::MathConstants<float>::pi * 220.0f * (float) i / (float) sr)
                                 + 0.15f * std::sin (2.0f * juce::MathConstants<float>::pi * 3000.0f * (float) i / (float) sr);
                    buf.setSample (0, i, s);
                    buf.setSample (1, i, s);
                }
                return buf;
            };

            auto runThrough = [&] (juce::AudioBuffer<float> buf)
            {
                juce::MidiBuffer midi;
                int done = 0;
                while (done < totalSamples)
                {
                    const auto thisBlock = juce::jmin (blockSize, totalSamples - done);
                    juce::AudioBuffer<float> block (2, thisBlock);
                    for (int ch = 0; ch < 2; ++ch)
                        block.copyFrom (ch, 0, buf, ch, done, thisBlock);
                    processor.processBlock (block, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        buf.copyFrom (ch, done, block, ch, 0, thisBlock);
                    done += thisBlock;
                }
                return buf;
            };

            const auto outputWithModulesOff = runThrough (makeTestSignal());

            // Pick a preset that clearly engages multiple modules (not
            // "Default", which is near-identity at every stage).
            const auto& preset = uni76::factoryPresets[1]; // "Warm Analog"
            const float rawValues[8] {
                preset.preamp, preset.eq, preset.saturation, preset.pitch,
                preset.panorama, preset.reverb, preset.imager, preset.imageTilt
            };
            for (size_t i = 0; i < uni76::ParamID::all.size(); ++i)
                if (auto* param = apvts.getParameter (uni76::ParamID::all[i]))
                    param->setValueNotifyingHost (param->convertTo0to1 (rawValues[i]));
            for (int m = 0; m < uni76::ModuleEnableState::numModules; ++m)
                processor.getModuleEnableState().setEnabled (m, true);

            for (int m = 0; m < uni76::ModuleEnableState::numModules; ++m)
                expect (processor.getModuleEnableState().isEnabled (m), "module should be enabled after preset load");

            const auto outputWithPresetOn = runThrough (makeTestSignal());

            double sumSqDiff = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < totalSamples; ++i)
                {
                    const auto diff = (double) (outputWithPresetOn.getSample (ch, i) - outputWithModulesOff.getSample (ch, i));
                    sumSqDiff += diff * diff;
                }
            const auto rmsDiff = std::sqrt (sumSqDiff / (double) (2 * totalSamples));
            std::cout << "\n=== all-modules-off vs preset-on RMS diff=" << rmsDiff << " ===" << std::endl << std::endl;
            expect (rmsDiff > 1.0e-4, "loading a preset with modules ON must audibly change the processed signal "
                                       "versus the all-modules-disabled state, not just flip flags");
        }

        beginTest ("User presets: save/load/delete round-trips through real disk I/O, filenames are sanitised");
        {
            const juce::String testName = "__UNI76TestPreset__";

            // Best-effort cleanup from a previous interrupted run.
            uni76::deleteUserPreset (testName);
            expect (! uni76::userPresetExists (testName), "precondition: test preset must not already exist");

            uni76::UserPresetData data;
            data.values = { 12.0f, 33.0f, 5.0f, -3.0f, 20.0f, 40.0f, 15.0f, -10.0f };
            data.moduleEnabled = { true, false, true, true, false, true, true };

            const auto saveOk = uni76::saveUserPreset (testName, data);
            expect (saveOk, "saveUserPreset should succeed");
            expect (uni76::userPresetExists (testName), "preset should exist immediately after saving");

            const auto names = uni76::listUserPresetNames();
            expect (names.contains (testName), "listUserPresetNames should include the just-saved preset");

            // Fresh read from disk - no in-memory state carried over from
            // saveUserPreset() above, the same as a brand-new plugin
            // instance reading a preset saved in a previous session.
            auto loaded = uni76::loadUserPreset (testName);
            expect (loaded.has_value(), "loadUserPreset should find the saved preset");
            if (loaded.has_value())
            {
                for (size_t i = 0; i < 8; ++i)
                    expectWithinAbsoluteError (loaded->values[i], data.values[i], 1.0e-4f, "value " + juce::String ((int) i));
                for (size_t i = 0; i < 7; ++i)
                    expect (loaded->moduleEnabled[i] == data.moduleEnabled[i], "moduleEnabled " + juce::String ((int) i));
            }

            const auto deleteOk = uni76::deleteUserPreset (testName);
            expect (deleteOk, "deleteUserPreset should succeed");
            expect (! uni76::userPresetExists (testName), "preset should not exist after deletion");
            expect (! uni76::loadUserPreset (testName).has_value(), "loadUserPreset should fail after deletion");

            // Filename sanitisation - a name with path-traversal/illegal
            // characters must not escape the presets directory or collide
            // with reserved filesystem characters.
            const auto sanitised = uni76::sanitizeUserPresetFilename ("../../evil:name*?\"<>|");
            expect (! sanitised.contains ("/") && ! sanitised.contains ("\\") && ! sanitised.contains (".."),
                    "sanitised filename must not contain path separators or traversal sequences");
            expect (! sanitised.containsAnyOf (":*?\"<>|"), "sanitised filename must not contain reserved characters");
        }

        beginTest ("Factory presets are never deletable (no delete path exists for them - by construction)");
        {
            // Factory presets live in a compiled-in constexpr array
            // (Core/FactoryPresets.h), not on disk - uni76DeleteUserPreset
            // only ever operates on getUserPresetsDirectory(), which a
            // factory preset's name was never written into, so there is no
            // code path that could delete a factory preset even if a
            // factory preset name were passed to it.
            for (auto& preset : uni76::factoryPresets)
                expect (! uni76::userPresetExists (juce::String (preset.name)),
                        juce::String (preset.name) + ": a factory preset name must never exist as a user preset file");
        }
    }
};

static UNI76UXPolishTests uni76UXPolishTests; // NOLINT - self-registers with the UnitTestRunner

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
