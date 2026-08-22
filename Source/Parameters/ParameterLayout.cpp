#include "ParameterLayout.h"
#include "ParameterIDs.h"

namespace uni76
{
    namespace
    {
        constexpr float defaultPercent = 50.0f;

        std::unique_ptr<juce::AudioParameterFloat> makePercentParameter (const char* id, const juce::String& name)
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

        params.push_back (makePercentParameter (ParamID::preamp,     "Preamp"));
        params.push_back (makePercentParameter (ParamID::eq,         "EQ"));
        params.push_back (makePercentParameter (ParamID::saturation, "Saturation"));
        params.push_back (makePercentParameter (ParamID::pitch,      "Pitch"));
        params.push_back (makePercentParameter (ParamID::panorama,   "Panorama"));
        params.push_back (makePercentParameter (ParamID::reverb,     "Reverb"));
        params.push_back (makePercentParameter (ParamID::imager,     "Imager"));

        return { params.begin(), params.end() };
    }
}
