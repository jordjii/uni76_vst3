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
    reverbParameter = apvts.getRawParameterValue (uni76::ParamID::reverb);
    imagerParameter = apvts.getRawParameterValue (uni76::ParamID::imager);
    imageTiltParameter = apvts.getRawParameterValue (uni76::ParamID::imageTilt);
    panRateParameter = apvts.getRawParameterValue (uni76::ParamID::panRate);
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
    verbProcessor.prepare (sampleRate, samplesPerBlock, numChannels);
    imagerProcessor.prepare (sampleRate, samplesPerBlock, numChannels);

    // EQ, PAN, VERB and IMAGE all add no algorithmic latency
    // (getLatencySamples() == 0 for each - PAN is a pure gain/filter
    // morph, no oversampling, no lookahead, no delay-based widening;
    // VERB's pre-delay/tank are wet-path effects, not a lookahead on the
    // direct signal - see docs/DSP_VERB.md; IMAGE is likewise a pure
    // gain/filter morph with no oversampling/lookahead/Haas-style delay
    // - see docs/DSP_IMAGE.md). PREAMP, SAT and PITCH each own
    // independent processing with their own real latency - the plugin's
    // total declared latency is their sum, since all seven run in series
    // in the signal chain and a host's plugin-delay-compensation needs
    // the combined delay, not just one stage's. See
    // updateReportedLatency() for why PITCH's own contribution is
    // conditional on its enabled flag while the others are not.
    updateReportedLatency();
}

void UNI76AudioProcessor::updateReportedLatency() noexcept
{
    // PITCH is a deliberate, documented exception - see
    // docs/DSP_PITCH.md's "Zero-latency bypass" section and
    // PitchProcessor.h's class comment. Its own algorithmic latency
    // (~140ms) is large enough that holding it open even while disabled -
    // which is PITCH's resting state in every factory preset, including
    // "Default" - was a real, reported bug: live MIDI/audio monitoring
    // through the plugin felt laggy with PITCH never actually engaged.
    // PREAMP/SAT's own always-on oversampling latency stays unconditional
    // here because it is architecturally always-on (a few samples,
    // independent of `enabled` by design - see their own
    // getLatencySamples() doc comments) and genuinely inaudible as
    // monitoring lag, unlike PITCH's.
    const auto pitchEnabled = moduleEnableState.isEnabled (3); // index 3 = pitch, see ModuleEnableState::propertyNames

    setLatencySamples (preampProcessor.getLatencySamples()
                        + eqProcessor.getLatencySamples()
                        + satProcessor.getLatencySamples()
                        + (pitchEnabled ? pitchProcessor.getLatencySamples() : 0)
                        + panoramaProcessor.getLatencySamples()
                        + verbProcessor.getLatencySamples()
                        + imagerProcessor.getLatencySamples());
}

void UNI76AudioProcessor::releaseResources()
{
    preampProcessor.reset();
    eqProcessor.reset();
    satProcessor.reset();
    pitchProcessor.reset();
    panoramaProcessor.reset();
    verbProcessor.reset();
    imagerProcessor.reset();
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

    // Every parameter/enabled value is read once here, up front, exactly
    // as before reordering was possible - only *which order* the seven
    // process() calls below happen in is now variable (see
    // Core/ChainOrder.h), not how each module's own inputs are read.
    const auto preampDrive = preampParameter != nullptr ? preampParameter->load() / 100.0f : 0.0f;
    const auto preampEnabled = moduleEnableState.isEnabled (0); // index 0 = preamp, see ModuleEnableState::propertyNames

    const auto eqTone = eqParameter != nullptr ? eqParameter->load() / 100.0f : 0.5f;
    const auto eqEnabled = moduleEnableState.isEnabled (1); // index 1 = eq, see ModuleEnableState::propertyNames

    const auto satHeat = saturationParameter != nullptr ? saturationParameter->load() / 100.0f : 0.0f;
    const auto satEnabled = moduleEnableState.isEnabled (2); // index 2 = saturation, see ModuleEnableState::propertyNames

    // PITCH's raw parameter value is already an integer semitone count
    // (-12..+12, see ParameterLayout.cpp's AudioParameterInt) - round
    // rather than truncate so float rounding on the atomic load can never
    // read e.g. 6.999999 as 6.
    const auto pitchSemitones = pitchParameter != nullptr ? (int) std::lround (pitchParameter->load()) : 0;
    const auto pitchEnabled = moduleEnableState.isEnabled (3); // index 3 = pitch, see ModuleEnableState::propertyNames

    // PAN/STEREO FIELD reads the raw `panorama` value as a plain 0..1
    // normalised width (not semitones/percent-of-something-else like
    // PITCH) - 0.5 (NATURAL) is both the parameter's own default and the
    // module's identity point, see docs/DSP_PAN.md.
    const auto panoramaWidth = panoramaParameter != nullptr ? panoramaParameter->load() / 100.0f : 0.5f;
    // PAN's nested RATE knob (see docs/DSP_PAN.md's "Motion rate" section) -
    // shares panoramaEnabled below (one bypass flag for the whole module,
    // same as IMAGE's imager+imageTilt pair).
    const auto panRate = panRateParameter != nullptr ? panRateParameter->load() / 100.0f : 0.35303f;
    const auto panoramaEnabled = moduleEnableState.isEnabled (4); // index 4 = panorama, see ModuleEnableState::propertyNames

    const auto reverbWet = reverbParameter != nullptr ? reverbParameter->load() / 100.0f : 0.0f;
    const auto reverbEnabled = moduleEnableState.isEnabled (5); // index 5 = reverb, see ModuleEnableState::propertyNames

    // IMAGE reads two independent raw parameter values - `imager` (0..1,
    // width/imaging amount) and `imageTilt` (-1..1 normalised, static
    // L/R balance) - see docs/DSP_IMAGE.md. Both share the single
    // `imagerEnabled` bypass flag (index 6): they are one module with
    // two axes, not two separate modules.
    const auto imageAmount = imagerParameter != nullptr ? imagerParameter->load() / 100.0f : 0.0f;
    const auto imageTilt = imageTiltParameter != nullptr ? imageTiltParameter->load() / 100.0f : 0.0f;
    const auto imagerEnabled = moduleEnableState.isEnabled (6); // index 6 = imager, see ModuleEnableState::propertyNames

    // Dispatch through the user's chosen chain order (drag-and-drop
    // pedalboard reordering - see Core/ChainOrder.h). Role indices match
    // ModuleEnableState::propertyNames' order, same as every `isEnabled()`
    // call above. A switch over a small fixed set of roles, not a
    // std::function table, so this stays allocation-free and realtime-safe.
    for (int position = 0; position < uni76::ChainOrder::numModules; ++position)
    {
        switch (chainOrder.roleAtPosition (position))
        {
            case 0: preampProcessor.process (buffer, preampDrive, preampEnabled); break;
            case 1: eqProcessor.process (buffer, eqTone, eqEnabled); break;
            case 2: satProcessor.process (buffer, satHeat, satEnabled); break;
            case 3: pitchProcessor.process (buffer, pitchSemitones, pitchEnabled); break;
            case 4: panoramaProcessor.process (buffer, panoramaWidth, panRate, panoramaEnabled); break;
            case 5: verbProcessor.process (buffer, reverbWet, reverbEnabled); break;
            case 6: imagerProcessor.process (buffer, imageAmount, imageTilt, imagerEnabled); break;
            default: break; // unreachable for a validated permutation - see ChainOrder::isValidPermutation()
        }
    }

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
double UNI76AudioProcessor::getTailLengthSeconds() const
{
    const auto reverbWet = reverbParameter != nullptr ? reverbParameter->load() / 100.0f : 0.0f;
    const auto reverbEnabled = moduleEnableState.isEnabled (5); // index 5 = reverb

    // verbWetGain(0) == 0.0 exactly (VerbCurves.h) - at reverbWet == 0 there
    // is no wet signal at all, so there is nothing decaying regardless of
    // what verbDecaySeconds(0) itself evaluates to.
    if (! reverbEnabled || reverbWet <= 0.0f)
        return 0.0;

    return (double) uni76::dsp::verbDecaySeconds (reverbWet);
}

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

    // Chain order (drag-and-drop pedalboard reordering) - same
    // outside-the-APVTS-tree persistence pattern as the module-enabled
    // flags above, one comma-joined string rather than 7 properties since
    // an order is inherently one unit - see Core/ChainOrder.h.
    {
        juce::StringArray parts;
        for (auto role : chainOrder.snapshot())
            parts.add (juce::String (role));
        state.setProperty (uni76::ChainOrder::stateProperty, parts.joinIntoString (","), nullptr);
    }

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

        // Pre-v5 states - genuinely old pre-DSP states *and* v4 states
        // saved under the since-retired MONO/NATURAL/WIDE contract alike
        // - saved `panorama` under a meaning that no longer exists: a
        // pre-DSP state's value was never sound-meaningful at all, and a
        // v4 state's "50% = NATURAL/identity" has no equivalent under the
        // current ORIGINAL(0%)/WIDE(50%)/MOTION(100%) contract (50% is
        // now WIDE, not identity). Force every pre-v5 state to the
        // current default (0%, ORIGINAL) rather than "best-effort"
        // preserving a number that means something different now - same
        // deliberate breaking-migration reasoning as PITCH's v3 bump: an
        // old (or v4-only) project that never touched PAN under the
        // current contract must not suddenly sound processed after this
        // update. `panorama`'s parameter ID and C++ type are unchanged -
        // only the stored value is forced, same mechanism as PITCH's
        // migration.
        if (loadedSchemaVersion < uni76::panoramaOriginalSchemaVersion)
        {
            auto panoramaParam = newState.getChildWithProperty ("id", juce::var (uni76::ParamID::panorama));
            if (panoramaParam.isValid())
                panoramaParam.setProperty ("value", 0.0, nullptr);
        }

        // A pre-v2 (or otherwise missing) flag defaults to enabled=true -
        // that's the correct migration for state saved before this flag
        // existed, with no separate branch needed.
        for (int i = 0; i < uni76::ModuleEnableState::numModules; ++i)
            moduleEnableState.setEnabled (i,
                (bool) newState.getProperty (uni76::ModuleEnableState::propertyNames[(size_t) i], true));

        // Chain order - a missing property (state saved before this round,
        // or from a corrupt/hand-edited file) falls back to the original
        // factory order rather than a partial/garbage permutation.
        {
            const auto saved = newState.getProperty (uni76::ChainOrder::stateProperty, juce::String()).toString();
            juce::StringArray parts;
            parts.addTokens (saved, ",", "");

            std::array<int, uni76::ChainOrder::numModules> parsed {};
            bool ok = parts.size() == uni76::ChainOrder::numModules;
            for (int i = 0; ok && i < uni76::ChainOrder::numModules; ++i)
            {
                if (! parts[i].containsOnly ("0123456789"))
                {
                    ok = false;
                    break;
                }
                parsed[(size_t) i] = parts[i].getIntValue();
            }

            if (ok && uni76::ChainOrder::isValidPermutation (parsed))
                chainOrder.setOrder (parsed);
            else
                chainOrder.resetToDefault();
        }

        apvts.replaceState (newState);

        // A restored state may set pitchEnabled differently than whatever
        // prepareToPlay() last computed the reported latency from (a host
        // can call setStateInformation() before or after prepareToPlay -
        // either order must end up correct) - see updateReportedLatency().
        updateReportedLatency();
    }
}

//==============================================================================
// This creates the platform-specific plugin instance for the JUCE VST3
// wrapper to host. Required entry point - do not rename or remove.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new UNI76AudioProcessor();
}
