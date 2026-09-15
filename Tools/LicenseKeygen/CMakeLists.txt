# ==============================================================================
#  UNI 76 offline license keygen (developer-only) - see main.cpp and
#  docs/LICENSING.md.
# ==============================================================================

add_executable(UNI76LicenseKeygen main.cpp)

target_link_libraries(UNI76LicenseKeygen
    PRIVATE
        juce::juce_core
        juce::juce_cryptography
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

target_compile_definitions(UNI76LicenseKeygen
    PRIVATE
        JUCE_STANDALONE_APPLICATION=1)
