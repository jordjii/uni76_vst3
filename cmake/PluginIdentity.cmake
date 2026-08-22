# ==============================================================================
#  UNI 76 - centralised plugin identity
#
#  This file is the single source of truth for every value that identifies
#  UNI 76 to a DAW/host: company name, bundle id, manufacturer/plugin 4CCs and
#  the marketing version. Nothing here may be generated or randomised - once
#  UNI 76 ships publicly, PLUGIN_MANUFACTURER_CODE / PLUGIN_CODE / BUNDLE_ID
#  become part of the plugin's identity in every host's plugin database and
#  must never change.
#
#  Do not duplicate these values elsewhere. If a new build script or CI job
#  needs them, make it `include()` this file instead of hard-coding strings.
# ==============================================================================

set(UNI76_COMPANY_NAME    "Nostalgia Audio")
set(UNI76_COMPANY_WEBSITE "https://nostalgiaaudio.com")
set(UNI76_COMPANY_EMAIL   "info@nostalgiaaudio.com")

set(UNI76_PRODUCT_NAME "UNI 76")

# Reverse-DNS bundle identifier used for the macOS bundle and as the basis of
# the WebView2 user-data folder name on Windows.
set(UNI76_BUNDLE_ID "com.nostalgiaaudio.uni76")

# JUCE plugin 4-character codes.
#   PLUGIN_MANUFACTURER_CODE requires at least one upper-case character.
#   PLUGIN_CODE requires exactly one upper-case character (GarageBand also
#   wants the first character upper-case and the rest lower-case, which this
#   satisfies).
set(UNI76_PLUGIN_MANUFACTURER_CODE "Nsta")
set(UNI76_PLUGIN_CODE              "Uni6")

# Marketing / build version. Keep in sync with the state schema version in
# Source/Core/PluginIdentity.h when preset formats change.
set(UNI76_VERSION "0.1.0")

# JUCE version pinned for reproducible builds. GIT_TAG is the exact commit
# that the "9.0.1" tag pointed to at the time this project was created, so a
# force-moved tag upstream cannot silently change what we build against.
set(UNI76_JUCE_GIT_REPOSITORY "https://github.com/juce-framework/JUCE.git")
set(UNI76_JUCE_GIT_TAG        "e18f7f506c0b96f2c738a0bcd7fe6467a5005ad8") # JUCE 9.0.1
