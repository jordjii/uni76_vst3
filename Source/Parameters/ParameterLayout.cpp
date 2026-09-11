#include "ParameterLayout.h"
#include "ParameterIDs.h"

namespace uni76
{
    namespace
    {
        std::unique_ptr<juce::AudioParameterFloat> makePercentParameter (const char* id, const juce::String& name, float defaultPercent)
        {
            return std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { id, ParamID::parameterVersionHint },
                name,
                juce::NormalisableRange<float> { 0.0f, 100.0f, 0.01f },
                defaultPercent,
                juce::AudioParameterFloatAttributes{}.withLabel ("%"));
        }

        // PITCH is genuinely discrete (see docs/DSP_PITCH.md): 25 fixed
        // integer semitone positions, -12..+12, default 0 (dead centre).
        // AudioParameterInt is the correct JUCE type for this - it reports
        // a real step count (25 valid states) to the host/VST3 layer,
        // unlike a float parameter with a UI-side snap. ParamID::pitch's
        // *string* ID is unchanged (see ParameterIDs.h) - only its C++
        // parameter type and range changed.
        std::unique_ptr<juce::AudioParameterInt> makePitchParameter()
        {
            return std::make_unique<juce::AudioParameterInt> (
                juce::ParameterID { ParamID::pitch, ParamID::parameterVersionHint },
                "Pitch",
                -12, 12, 0,
                juce::AudioParameterIntAttributes{}.withLabel ("ST"));
        }

        // IMAGE TILT is a static L/R stereo-image balance, -100 (LEFT) ..
        // +100 (RIGHT), default 0 (CENTER) - see docs/DSP_IMAGE.md. Kept
        // as a float (not an AudioParameterInt like PITCH) since a
        // continuous balance control has no natural discrete-step count
        // the way PITCH's 25 semitone positions do.
        std::unique_ptr<juce::AudioParameterFloat> makeImageTiltParameter()
        {
            return std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { ParamID::imageTilt, ParamID::parameterVersionHint },
                "Image Tilt",
                juce::NormalisableRange<float> { -100.0f, 100.0f, 0.01f },
                0.0f);
        }

        // IMAGE AMOUNT (live-testing follow-up round: bipolar redesign) -
        // -100 (MONO) .. 0 (CENTER, identity) .. +100 (STEREO), default 0.
        // Was a plain 0..100% parameter (ORIGINAL(0%)/WIDE(100%)) before
        // this round - see docs/DSP_IMAGE.md's "Bipolar redesign" section
        // for the full reasoning. Deliberately NOT given a schema-version
        // bump the way PITCH's v3 or PAN's v5 range changes were: those
        // forced a reset because the *old* value's meaning no longer
        // existed anywhere in the new contract (a v4 PAN "50%" meant
        // NATURAL/identity, which isn't 50% under the new contract
        // either). Here the old [0,100] domain is a strict *subset* of
        // the new [-100,100] domain, and the positive half's curve is
        // byte-for-byte unchanged (see ImagerCurves.h) - so an old saved
        // value loads at the exact same real number under the new range
        // and produces the exact same sound it always did; there is
        // nothing to migrate. Verified by a dedicated test loading a
        // hand-built legacy state with `imager` values across its full
        // old 0..100 range.
        std::unique_ptr<juce::AudioParameterFloat> makeImagerParameter()
        {
            return std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { ParamID::imager, ParamID::parameterVersionHint },
                "Imager",
                juce::NormalisableRange<float> { -100.0f, 100.0f, 0.01f },
                0.0f);
        }
    }

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        // EQ defaults to its centred/flat "PHONE" position (50%). Every
        // other module - including PAN/`panorama` - defaults to fully off
        // (0%), matching a console where drive/saturation/pitch/width/
        // space/image all start at zero. PAN's 0% is its ORIGINAL
        // (bit-exact identity) position - see docs/DSP_PAN.md. An earlier
        // revision of this module briefly used a 50% ("NATURAL") default
        // under a since-superseded MONO/NATURAL/WIDE product contract;
        // that contract and its default are retired - see
        // PluginIdentity.h's panoramaOriginalSchemaVersion.
        params.push_back (makePercentParameter (ParamID::preamp,     "Preamp",     0.0f));
        params.push_back (makePercentParameter (ParamID::eq,         "EQ",         50.0f));
        params.push_back (makePercentParameter (ParamID::saturation, "Saturation", 0.0f));
        params.push_back (makePitchParameter());
        params.push_back (makePercentParameter (ParamID::panorama,   "Panorama",   0.0f));
        params.push_back (makePercentParameter (ParamID::reverb,     "Reverb",     0.0f));
        params.push_back (makeImagerParameter());
        params.push_back (makeImageTiltParameter());

        // PAN's nested RATE knob (see docs/DSP_PAN.md's "Motion rate"
        // section, Source/DSP/PanoramaCurves.h's panRateHz()). 35.303% is
        // not an arbitrary "middle" default - it is the exact normalised
        // position that solves panRateHz(t) == 0.3Hz, the fixed LFO speed
        // every PAN preset/session relied on before this parameter
        // existed, so nothing that never touches the new RATE knob
        // changes speed. Verified by a dedicated test in
        // Tests/PluginTests.cpp against PanoramaCurves.h's own curve.
        params.push_back (makePercentParameter (ParamID::panRate, "Pan Rate", 35.303f));

        // VERB's nested DRIVE knob (see docs/DSP_VERB.md's "Drive (nested
        // knob)" section, Source/DSP/VerbCurves.h). 0% default = exactly
        // today's pre-existing tiny fixed send/return coloration - a
        // session/preset that never touches DRIVE sounds identical to
        // before this parameter existed.
        params.push_back (makePercentParameter (ParamID::verbDrive, "Verb Drive", 0.0f));

        return { params.begin(), params.end() };
    }
}
