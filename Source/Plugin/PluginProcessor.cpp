#include "PluginProcessor.h"

#include <cmath>

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
    pitchParameter = apvts.getRawParameterValue (uni76::ParamID::pitch);
    panoramaParameter = apvts.getRawParameterValue (uni76::ParamID::panorama);
}

UNI76AudioProcessor::~UNI76AudioProcessor() = default;

//==============================================================================
void UNI76AudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const auto numChannels = juce::jmax (1, getTotalNumOutputChannels());

    preampProcessor.prepare (sampleRate, samplesPerBlock, numChannels);
    eqProcessor.prepare (sampleRate, samplesPerBlock, numChannels);
    satProcessor.prepare (sampleRate, samplesPerBlock, numChannels);
    pitchProcessor.prepare (sampleRate, samplesPerBlock, numChannels);
    panoramaProcessor.prepare (sampleRate, samplesPerBlock, numChannels);

    // EQ and PAN both add no algorithmic latency (getLatencySamples() ==
    // 0 for each - PAN is a pure gain/filter morph, no oversampling, no
    // lookahead, no delay-based widening). PREAMP, SAT and PITCH each own
    // independent processing with their own real latency - the plugin's
    // total declared latency is their sum, since all five run in series
    // in the signal chain and a host's plugin-delay-compensation needs
    // the combined delay, not just one stage's.
    setLatencySamples (preampProcessor.getLatencySamples()
                        + eqProcessor.getLatencySamples()
                        + satProcessor.getLatencySamples()
                        + pitchProcessor.getLatencySamples()
                        + panoramaProcessor.getLatencySamples());
}

void UNI76AudioProcessor::releaseResources()
{
    preampProcessor.reset();
    eqProcessor.reset();
    satProcessor.reset();
    pitchProcessor.reset();
    panoramaProcessor.reset();
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

    // PITCH's raw parameter value is already an integer semitone count
    // (-12..+12, see ParameterLayout.cpp's AudioParameterInt) - round
    // rather than truncate so float rounding on the atomic load can never
    // read e.g. 6.999999 as 6.
    const auto pitchSemitones = pitchParameter != nullptr ? (int) std::lround (pitchParameter->load()) : 0;
    const auto pitchEnabled = moduleEnableState.isEnabled (3); // index 3 = pitch, see ModuleEnableState::propertyNames

    pitchProcessor.process (buffer, pitchSemitones, pitchEnabled);

    // PAN/STEREO FIELD reads the raw `panorama` value as a plain 0..1
    // normalised width (not semitones/percent-of-something-else like
    // PITCH) - 0.5 (NATURAL) is both the parameter's own default and the
    // module's identity point, see docs/DSP_PAN.md.
    const auto panoramaWidth = panoramaParameter != nullptr ? panoramaParameter->load() / 100.0f : 0.5f;
    const auto panoramaEnabled = moduleEnableState.isEnabled (4); // index 4 = panorama, see ModuleEnableState::propertyNames

    panoramaProcessor.process (buffer, panoramaWidth, panoramaEnabled);

    // Reverb/Imager remain a strict passthrough at this stage - see
    // CLAUDE.md.

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

        const int loadedSchemaVersion =
            newState.getProperty (uni76::stateSchemaVersionProperty, 1);

        // Pre-v3 states saved `pitch` as a raw 0..100 number under the
        // old percent-based parameter - it has no semitone meaning and
        // must never be reinterpreted as one (verified against the real
        // APVTS save format - see docs/DSP_PITCH.md's "Parameter and
        // state migration" section). Strip it before apvts.replaceState()
        // so the new -12..+12 AudioParameterInt falls back to its own
        // default (0 ST) instead of clamping/misreading the old value.
        // A user who never touched PITCH keeps hearing 0 ST after update.
        //
        // This is a DELIBERATE, breaking semantic migration - not an
        // oversight. The old `pitch` never had any audible effect (it
        // predates PitchProcessor entirely, see CLAUDE.md's DSP history),
        // so no saved project's *sound* depends on its old raw value in
        // any way; the only thing that could go wrong is silently
        // reinterpreting an old UI-only number as a real semitone shift
        // and having an old project suddenly transpose itself on load.
        // Forcing every pre-v3 state to 0 ST is what guarantees that
        // can't happen - it is intentionally not "best effort" preserved.
        if (loadedSchemaVersion < uni76::pitchDiscreteSchemaVersion)
        {
            auto pitchParam = newState.getChildWithProperty ("id", juce::var (uni76::ParamID::pitch));
            if (pitchParam.isValid())
                pitchParam.setProperty ("value", 0.0, nullptr);
        }

        // Pre-v4 states saved `panorama` under its old 0%-default
        // AudioParameterFloat, from before any DSP read it - that old
        // value (typically 0.0, or whatever a user happened to leave the
        // then-inert knob at) never meant "mono" and must not suddenly be
        // interpreted as MONO now that 0% genuinely collapses the stereo
        // image. Same deliberate breaking-migration reasoning as PITCH's
        // v3 bump above: force every pre-v4 state to the new default
        // (50%, NATURAL) rather than "best-effort" preserving a number
        // that was never sound-meaningful, so an old project that never
        // touched PAN can't suddenly play in mono after this update.
        // `panorama`'s parameter ID and C++ type are unchanged - only the
        // stored value is forced, same mechanism as PITCH's migration.
        if (loadedSchemaVersion < uni76::panoramaNaturalSchemaVersion)
        {
            auto panoramaParam = newState.getChildWithProperty ("id", juce::var (uni76::ParamID::panorama));
            if (panoramaParam.isValid())
                panoramaParam.setProperty ("value", 50.0, nullptr);
        }

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
