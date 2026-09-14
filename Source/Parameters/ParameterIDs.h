#pragma once

#include <array>

/*
    Stable, centralised parameter identifiers for UNI 76.

    These strings are persisted in saved presets and referenced by DAW
    automation lanes. They must never be renamed once UNI 76 has shipped
    publicly - renaming an ID breaks every saved session and automation
    lane that references it.

    There were exactly 7 public parameters through the plugin's initial
    development (see CLAUDE.md's history). `imageTilt` is a deliberate,
    explicit exception to that "one knob per module" rule, added once
    IMAGE itself gained real DSP: IMAGE now has two independent axes -
    `imager` (frequency-dependent width/imaging amount) and `imageTilt`
    (a static stereo left/right balance-tilt on top of that same image) -
    see docs/DSP_IMAGE.md. Low Cut / High Cut remain internal to PREAMP,
    not separate automatable parameters.

    `panRate` (9th parameter, PAN's nested RATE knob - see docs/DSP_PAN.md's
    "Motion rate" section) is the second such deliberate exception: PAN's
    motion LFO speed was a fixed constant (~0.3Hz) until this round, and is
    now a real, automatable, user-adjustable axis alongside `panorama`'s
    existing width/motion-depth control - the same "genuinely independent
    second axis on one module, not one knob controlling two things" reasoning
    IMAGE's `imageTilt` addition already established as this project's own
    precedent for growing past 8 parameters.

    `verbDrive` (10th parameter, VERB's nested DRIVE knob - see
    docs/DSP_VERB.md's "Drive (nested knob)" section) is the third:
    VERB's analog send/return coloration was a fixed, deliberately tiny
    pair of tanh() constants until this round ("texture, not a second SAT
    module"); DRIVE now scales both stages together, from that same
    tiny-texture resting point up to a genuinely hot, audibly-driven
    plate - referencing a real reference plugin's (Vynl Audio Voyager-
    Verb) own nested-knob DRIVE control.

    `delay`/`delayFeedback`/`delayDivision`/`delayStereo`/`delayPingPong`
    (11th-15th parameters, the new DELAY module - see docs/DSP_DELAY.md)
    added 2026-09-14: a tempo-synced (BPM-locked, never free-running-ms)
    delay/echo module, the 8th DSP module and the first one whose own
    public surface needs more than a plain 0..100% float per parameter -
    `delayDivision` is a genuine `AudioParameterChoice` (5 fixed note
    divisions) and `delayStereo`/`delayPingPong` are genuine
    `AudioParameterBool`s, bridged to the WebView through JUCE's own
    WebComboBoxRelay/WebToggleButtonRelay (the same already-vendored
    `Resources/Web/juce_webview.js` module that the existing WebSliderRelay
    bridge uses - not a new dependency, just a different relay/attachment
    pair from the same JUCE mechanism). `delay` (MIX) and `delayFeedback`
    are plain 0..100%/0..95% floats, the same nested-outer/inner-knob
    pattern `panRate`/`verbDrive` already established.
*/

namespace uni76::ParamID
{
    inline constexpr const char* preamp     = "preamp";
    inline constexpr const char* eq         = "eq";
    inline constexpr const char* saturation = "saturation";
    inline constexpr const char* pitch      = "pitch";
    inline constexpr const char* panorama   = "panorama";
    inline constexpr const char* reverb     = "reverb";
    inline constexpr const char* imager     = "imager";
    inline constexpr const char* imageTilt  = "imageTilt";
    inline constexpr const char* panRate    = "panRate";
    inline constexpr const char* verbDrive  = "verbDrive";

    // DELAY (see docs/DSP_DELAY.md) - the 8th DSP module, added 2026-09-14.
    inline constexpr const char* delay          = "delay";
    inline constexpr const char* delayFeedback  = "delayFeedback";
    inline constexpr const char* delayDivision  = "delayDivision";
    inline constexpr const char* delayStereo    = "delayStereo";
    inline constexpr const char* delayPingPong  = "delayPingPong";

    /** Version tag passed to juce::ParameterID for every parameter below.
        JUCE mixes this into the VST3 parameter hash; bump it only if a
        parameter's meaning changes in a way that should be treated as a
        new automation target by hosts.
    */
    inline constexpr int parameterVersionHint = 1;

    /** All parameter IDs, for iteration (tests, UI wiring, etc). */
    inline constexpr std::array<const char*, 15> all
    {
        preamp, eq, saturation, pitch, panorama, reverb, imager, imageTilt, panRate, verbDrive,
        delay, delayFeedback, delayDivision, delayStereo, delayPingPong
    };
}
