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
    }

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

        // EQ defaults to its centred/flat "PHONE" position (50%) - every
        // other module defaults to fully off (0%), matching a console
        // where drive/saturation/pitch/width/space/image all start at
        // zero and only tone defaults to neutral rather than an extreme.
        params.push_back (makePercentParameter (ParamID::preamp,     "Preamp",     0.0f));
        params.push_back (makePercentParameter (ParamID::eq,         "EQ",         50.0f));
        params.push_back (makePercentParameter (ParamID::saturation, "Saturation", 0.0f));
        params.push_back (makePercentParameter (ParamID::pitch,      "Pitch",      0.0f));
        params.push_back (makePercentParameter (ParamID::panorama,   "Panorama",   0.0f));
        params.push_back (makePercentParameter (ParamID::reverb,     "Reverb",     0.0f));
        params.push_back (makePercentParameter (ParamID::imager,     "Imager",     0.0f));

        return { params.begin(), params.end() };
    }
}
