#include "PluginProcessor.h"
#include "Core/PluginIdentity.h"
#include "Parameters/ParameterIDs.h"
#include "Parameters/ParameterLayout.h"
#include "UI/WebUIEditor.h"

//==============================================================================
UNI76AudioProcessor::UNI76AudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", uni76::createParameterLayout())
{
    preampParameter = apvts.getRawParameterValue (uni76::ParamID::preamp);
    eqParameter = apvts.getRawParameterValue (uni76::ParamID::eq);
    saturationParameter = apvts.getRawParameterValue (uni76::ParamID::saturation);
}

UNI76AudioProcessor::~UNI76AudioProcessor() = default;

//==============================================================================
void UNI76AudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const auto numChannels = juce::jmax (1, getTotalNumOutputChannels());

    preampProcessor.prepare (sampleRate, samplesPerBlock, numChannels);
    eqProcessor.prepare (sampleRate, samplesPerBlock, numChannels);
    satProcessor.prepare (sampleRate, samplesPerBlock, numChannels);

    // EQ adds no algorithmic latency (getLatencySamples() == 0). PREAMP
    // and SAT each own an independent oversampling instance and each
    // report their own real latency - the plugin's total declared latency
    // is their sum, since the two run in series in the signal chain and a
    // host's plugin-delay-compensation needs the combined delay, not just
    // one stage's.
    setLatencySamples (preampProcessor.getLatencySamples()
                        + eqProcessor.getLatencySamples()
                        + satProcessor.getLatencySamples());
}

void UNI76AudioProcessor::releaseResources()
{
    preampProcessor.reset();
    eqProcessor.reset();
    satProcessor.reset();
}

bool UNI76AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainIn  = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;

    // Mono -> mono and stereo -> stereo only; no channel-count conversion.
    return mainIn == mainOut;
}

void UNI76AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midiMessages);

    // Measured before the processing chain.
    inputLevelMeter.pushBlock (buffer);

    const auto preampDrive = preampParameter != nullptr ? preampParameter->load() / 100.0f : 0.0f;
    const auto preampEnabled = moduleEnableState.isEnabled (0); // index 0 = preamp, see ModuleEnableState::propertyNames

    preampProcessor.process (buffer, preampDrive, preampEnabled);

    const auto eqTone = eqParameter != nullptr ? eqParameter->load() / 100.0f : 0.5f;
    const auto eqEnabled = moduleEnableState.isEnabled (1); // index 1 = eq, see ModuleEnableState::propertyNames

    eqProcessor.process (buffer, eqTone, eqEnabled);

    const auto satHeat = saturationParameter != nullptr ? saturationParameter->load() / 100.0f : 0.0f;
    const auto satEnabled = moduleEnableState.isEnabled (2); // index 2 = saturation, see ModuleEnableState::propertyNames

    satProcessor.process (buffer, satHeat, satEnabled);

    // Pitch/Panorama/Reverb/Imager remain a strict passthrough at this
    // stage - see CLAUDE.md.

    // Measured after the processing chain - now meaningfully different
    // from the input reading whenever PREAMP is enabled and driven.
    outputLevelMeter.pushBlock (buffer);
}

//==============================================================================
juce::AudioProcessorEditor* UNI76AudioProcessor::createEditor()
{
    return new UNI76AudioProcessorEditor (*this);
}

bool UNI76AudioProcessor::hasEditor() const
{
    return true;
}

//==============================================================================
const juce::String UNI76AudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool UNI76AudioProcessor::acceptsMidi() const     { return false; }
bool UNI76AudioProcessor::producesMidi() const    { return false; }
bool UNI76AudioProcessor::isMidiEffect() const    { return false; }
double UNI76AudioProcessor::getTailLengthSeconds() const { return 0.0; }

//==============================================================================
int UNI76AudioProcessor::getNumPrograms()                            { return 1; }
int UNI76AudioProcessor::getCurrentProgram()                         { return 0; }
void UNI76AudioProcessor::setCurrentProgram (int)                    {}
const juce::String UNI76AudioProcessor::getProgramName (int)         { return {}; }
void UNI76AudioProcessor::changeProgramName (int, const juce::String&) {}

//==============================================================================
void UNI76AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty (uni76::stateSchemaVersionProperty, uni76::stateSchemaVersion, nullptr);

    // Module-enabled flags live as plain properties on the same saved
    // ValueTree as the APVTS parameters, but are NOT APVTS parameters
    // themselves - see Core/ModuleEnableState.h for why.
    for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
        state.setProperty (uni76::ModuleEnableState::propertyNames[(size_t) i],
                            moduleEnableState.isEnabled (i), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void UNI76AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        auto newState = juce::ValueTree::fromXml (*xml);

        if (! newState.isValid() || newState.getType() != apvts.state.getType())
            return;

        // Future preset migrations branch on this property if needed.
        [[maybe_unused]] const int loadedSchemaVersion =
            newState.getProperty (uni76::stateSchemaVersionProperty, 1);

        // A pre-v2 (or otherwise missing) flag defaults to enabled=true -
        // that's the correct migration for state saved before this flag
        // existed, with no separate branch needed.
        for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
            moduleEnableState.setEnabled (i,
                (bool) newState.getProperty (uni76::ModuleEnableState::propertyNames[(size_t) i], true));

        apvts.replaceState (newState);
    }
}

//==============================================================================
// This creates the platform-specific plugin instance for the JUCE VST3
// wrapper to host. Required entry point - do not rename or remove.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new UNI76AudioProcessor();
}
