#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

class UNI76AudioProcessor;

/*
    UNI 76's WebView editor.

    Renders the embedded HTML/CSS/JS UI (Resources/Web) inside a
    WebBrowserComponent and binds each of the 7 parameters to it via JUCE's
    WebSliderRelay / WebSliderParameterAttachment bridge, so that:
      - moving an HTML control updates the JUCE parameter (-> DAW automation
        and state).
      - automation or state changes from the DAW/host update the HTML
        control.

    Resizable within a fixed 3:2 aspect ratio - see the constructor.
*/
class UNI76AudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit UNI76AudioProcessorEditor (UNI76AudioProcessor&);
    ~UNI76AudioProcessorEditor() override;

    void resized() override;

    int getControlParameterIndex (Component&) override;

private:
    // Only ever navigates to our own embedded resource root - the frontend
    // cannot be redirected to an external site.
    struct SinglePageBrowser final : juce::WebBrowserComponent
    {
        using WebBrowserComponent::WebBrowserComponent;
        bool pageAboutToLoad (const juce::String& newURL) override;
    };

    UNI76AudioProcessor& processor;

    juce::WebSliderRelay preampRelay     { "preamp" };
    juce::WebSliderRelay eqRelay         { "eq" };
    juce::WebSliderRelay saturationRelay { "saturation" };
    juce::WebSliderRelay pitchRelay      { "pitch" };
    juce::WebSliderRelay panoramaRelay   { "panorama" };
    juce::WebSliderRelay reverbRelay     { "reverb" };
    juce::WebSliderRelay imagerRelay     { "imager" };

    juce::WebControlParameterIndexReceiver controlParameterIndexReceiver;

    SinglePageBrowser webView;

    juce::WebSliderParameterAttachment preampAttachment;
    juce::WebSliderParameterAttachment eqAttachment;
    juce::WebSliderParameterAttachment saturationAttachment;
    juce::WebSliderParameterAttachment pitchAttachment;
    juce::WebSliderParameterAttachment panoramaAttachment;
    juce::WebSliderParameterAttachment reverbAttachment;
    juce::WebSliderParameterAttachment imagerAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UNI76AudioProcessorEditor)
};
