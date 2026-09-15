# ==============================================================================
#  UNI 76 automated tests.
#
#  Uses JUCE's own juce::UnitTest / UnitTestRunner (from juce_core) rather
#  than pulling in a separate test framework - it's already a dependency of
#  the plugin and is more than sufficient for this stage's checks.
# ==============================================================================

add_executable(UNI76Tests PluginTests.cpp)

target_include_directories(UNI76Tests
    PRIVATE
        "${CMAKE_SOURCE_DIR}/Source")

target_link_libraries(UNI76Tests
    PRIVATE
        UNI76
        juce::juce_audio_processors
        juce::juce_cryptography
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# UNI76 re-exports JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING as an INTERFACE
# compile definition (this is JUCE's own CMake plugin-target machinery, not
# something this project opts into per-target) - so any consumer that links
# UNI76 and also compiles the juce_gui_extra module, such as this test
# executable, needs the WebView2 SDK include path available too, even
# though the tests never touch WebBrowserComponent themselves.
if(WIN32 AND TARGET juce::juce_webview2)
    target_link_libraries(UNI76Tests PRIVATE juce::juce_webview2)
endif()

add_test(NAME UNI76UnitTests COMMAND UNI76Tests)

# ==============================================================================
#  RC1 blocker 3: accelerated offline long-run soak test (see
#  Tests/SoakTest.cpp and docs/FULL_DSP_AUDIT.md's "RC1 blocker closure"
#  section). Deliberately a SEPARATE executable from UNI76Tests - it
#  processes hours of equivalent audio and would otherwise make every
#  routine test run multi-minute. Not wired into `add_test`/CTest or CI;
#  run on demand for release-readiness verification only.
# ==============================================================================

add_executable(UNI76Soak SoakTest.cpp)

target_include_directories(UNI76Soak
    PRIVATE
        "${CMAKE_SOURCE_DIR}/Source")

target_link_libraries(UNI76Soak
    PRIVATE
        UNI76
        juce::juce_audio_processors
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(WIN32 AND TARGET juce::juce_webview2)
    target_link_libraries(UNI76Soak PRIVATE juce::juce_webview2)
endif()

if(WIN32)
    target_link_libraries(UNI76Soak PRIVATE psapi)
endif()
