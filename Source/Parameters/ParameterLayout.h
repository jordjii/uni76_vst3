#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/*
    Builds the AudioProcessorValueTreeState parameter layout for UNI 76.

    All 7 public parameters are exposed to hosts as 0..100 (%) ranged floats
    with a default of 50%, per the product spec for this stage. None of them
    affect the audio signal yet - see PluginProcessor::processBlock.
*/

namespace uni76
{
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
}
