# ==============================================================================
#  UNI 76 plugin target
# ==============================================================================

juce_add_plugin(UNI76
    VERSION                      "${UNI76_VERSION}"
    COMPANY_NAME                 "${UNI76_COMPANY_NAME}"
    COMPANY_WEBSITE               "${UNI76_COMPANY_WEBSITE}"
    COMPANY_EMAIL                 "${UNI76_COMPANY_EMAIL}"
    BUNDLE_ID                    "${UNI76_BUNDLE_ID}"
    PLUGIN_MANUFACTURER_CODE      ${UNI76_PLUGIN_MANUFACTURER_CODE}
    PLUGIN_CODE                   ${UNI76_PLUGIN_CODE}
    PRODUCT_NAME                 "${UNI76_PRODUCT_NAME}"
    FORMATS                       VST3
    VST3_CATEGORIES               Fx
    IS_SYNTH                      FALSE
    NEEDS_MIDI_INPUT              FALSE
    NEEDS_MIDI_OUTPUT             FALSE
    IS_MIDI_EFFECT                FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS   TRUE
    COPY_PLUGIN_AFTER_BUILD       FALSE   # production builds never auto-install; see Packaging/*/dev-install scripts
    NEEDS_WEB_BROWSER             TRUE
    NEEDS_WEBVIEW2                TRUE)   # required so the WebView2 SDK headers/lib are available on Windows; see docs/BUILD.md

target_sources(UNI76
    PRIVATE
        PluginProcessor.cpp
        PluginProcessor.h

        ../Core/PluginIdentity.h
        ../Core/LevelMeter.h
        ../Core/ModuleEnableState.h
        ../Core/MeterEnvelope.h

        ../Parameters/ParameterIDs.h
        ../Parameters/ParameterLayout.h
        ../Parameters/ParameterLayout.cpp

        ../DSP/Biquad.h
        ../DSP/PreampCurves.h
        ../DSP/PreampProcessor.h
        ../DSP/PreampProcessor.cpp
        ../DSP/EqCurves.h
        ../DSP/EqProcessor.h
        ../DSP/EqProcessor.cpp
        ../DSP/SatCurves.h
        ../DSP/SatProcessor.h
        ../DSP/SatProcessor.cpp
        ../DSP/Pitch.h
        ../DSP/Panorama.h
        ../DSP/Reverb.h
        ../DSP/Imager.h

        ../UI/WebUIEditor.h
        ../UI/WebUIEditor.cpp
        ../UI/WebResourceProvider.h
        ../UI/WebResourceProvider.cpp)

target_include_directories(UNI76
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Embedded web UI (Resources/Web/*) compiled into the binary via JUCE's
# normal binary-data resource pipeline. The running plugin never reads from
# the Resources/Web folder on disk.
juce_add_binary_data(UNI76WebResources
    SOURCES
        "${CMAKE_SOURCE_DIR}/Resources/Web/index.html"

        "${CMAKE_SOURCE_DIR}/Resources/Web/tokens.css"
        "${CMAKE_SOURCE_DIR}/Resources/Web/reset.css"
        "${CMAKE_SOURCE_DIR}/Resources/Web/shell.css"
        "${CMAKE_SOURCE_DIR}/Resources/Web/header.css"
        "${CMAKE_SOURCE_DIR}/Resources/Web/modules.css"
        "${CMAKE_SOURCE_DIR}/Resources/Web/knobs.css"
        "${CMAKE_SOURCE_DIR}/Resources/Web/scales.css"
        "${CMAKE_SOURCE_DIR}/Resources/Web/meters.css"
        "${CMAKE_SOURCE_DIR}/Resources/Web/responsive.css"

        "${CMAKE_SOURCE_DIR}/Resources/Web/app.js"
        "${CMAKE_SOURCE_DIR}/Resources/Web/knob.js"
        "${CMAKE_SOURCE_DIR}/Resources/Web/aux_visuals.js"
        "${CMAKE_SOURCE_DIR}/Resources/Web/meters.js"
        "${CMAKE_SOURCE_DIR}/Resources/Web/module_power.js"
        "${CMAKE_SOURCE_DIR}/Resources/Web/juce_webview.js")

target_compile_definitions(UNI76
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1)
        # JUCE_DISPLAY_SPLASH_SCREEN is intentionally left at its default
        # (splash shown). Disabling it requires a commercial JUCE license -
        # see docs/RELEASE.md for the pre-public-release licensing checklist.

if(WIN32)
    # PRIVATE: only UNI76's own translation units (which is where the
    # WebView2-backed editor lives) need this. Consumers such as UNI76Tests
    # never touch WebBrowserComponent and must not be forced to see the
    # WebView2 SDK include path.
    target_compile_definitions(UNI76
        PRIVATE
            # Static WebView2Loader linking: no loader DLL needs to be
            # shipped or located at runtime. See docs/BUILD.md.
            JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)
endif()

target_link_libraries(UNI76
    PRIVATE
        UNI76WebResources
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
