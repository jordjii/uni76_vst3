# UNI 76 - RC1 FL Studio manual smoke test

This is a **manual gate for the product owner** to run before any public
release - not something this session's automated tooling can perform.
FL Studio 21 is genuinely installed on the development machine, but no
tool available to Claude Code drives a third-party desktop application's
own GUI (only this project's own browser-pane-based tooling and
purpose-built JUCE scratch hosts, neither of which can substitute for a
real interactive DAW session). See
[docs/FULL_DSP_AUDIT.md](FULL_DSP_AUDIT.md) section 37 for that
limitation's own record.

Do not treat this checklist as passed until a human has actually run it.

## Prerequisites

- UNI 76 installed via the RC1 installer (`Packaging/Windows/Output/
  UNI76-Windows-x64-RC1-Setup.exe`), **not** a dev-install copy - see
  [docs/FULL_DSP_AUDIT.md](FULL_DSP_AUDIT.md)'s RC1 report for the
  install/uninstall dry-run this session already performed on the same
  machine.
- FL Studio 21.

## Checklist

1. **Scan.** Open FL Studio -> Options -> Manage Plugins (Plugin
   Manager) -> Find installed plugins / Rescan. Confirm the scan
   completes without FL Studio reporting a crash or a "plugin failed to
   load" entry for UNI 76.
2. **Find UNI 76.** Confirm it appears in the plugin database under
   Nostalgia Audio / Fx, with the correct name "UNI 76".
3. **Add to mixer insert.** Add UNI 76 as an effect on a mixer insert
   track (not the master, so it can be bypassed/removed independently).
4. **Open GUI.** Confirm the WebView2-based interface renders correctly
   at its default size - all 7 modules, header, footer, meters, no
   blank/black window, no error dialog.
5. **INPUT/OUTPUT meters.** Play audio through the insert; confirm both
   meters show real, moving activity (not stuck at zero, not stuck at
   full).
6. **Turn every knob.** PREAMP, EQ, SAT, PITCH, PAN, VERB, IMAGE - confirm
   each responds visually and audibly, and that FL Studio's own
   automation-recording indicator lights up during the drag (confirms
   host<->parameter binding, not just a purely-visual UI).
7. **PITCH -12 / 0 / +12.** Confirm all three land exactly on their
   marked positions and the audio shifts by a full octave down/up/none.
8. **PAN MOTION.** Set PAN toward 100% and confirm the stereo field
   audibly rotates/moves over a few seconds, not just widens statically.
9. **VERB.** Set VERB to a non-zero value and confirm an audible plate
   reverb tail appears; confirm it stops cleanly when set back to 0%.
10. **IMAGE FIELD.** Drag the small square field pad inside the IMAGE
    module and confirm both the pad's puck position and the main IMAGE
    knob move together; confirm the L/R balance shifts when dragging
    horizontally.
11. **Factory presets.** Click PRESET, confirm a dropdown list appears
    with the ~10 factory preset names (Default, Warm Analog, Dark
    Vintage, Telephone Plate, Wide Vintage, Motion Space, Focused
    Stereo, Deep Plate, Hot Console, Clean Wide - see
    Source/Core/FactoryPresets.h). Select at least 2-3 different presets
    and confirm every knob visibly jumps to that preset's values.
11a. **A/B.** Click A/B once, tweak a knob, click A/B again - confirm the
    sound/knob positions swap to the other slot's values, and clicking a
    third time swaps back including the tweak you just made. This is a
    session-local convenience only (not saved with the project) - see
    Source/UI/WebUIEditor.h's `ABSnapshot` for the design. Automated
    coordinate-click testing of this control was attempted this session
    and found unreliable in the dev environment (see
    docs/FULL_DSP_AUDIT.md's RC1 report) - this manual step is the real
    verification.
12. **Record automation.** With a preset loaded, record a short
    automation move on at least one parameter (e.g. draw an automation
    clip for PREAMP or PAN) and confirm playback reproduces it.
13. **Save FL project.** Save the project (.flp) with UNI 76 in its
    current (non-default) state.
14. **Close FL Studio** completely.
15. **Reopen the project.** Confirm UNI 76 reloads with the exact same
    knob positions, the same selected preset's sound, and any recorded
    automation intact.
16. **Verify state restore audibly**, not just visually - play back and
    confirm the processed sound matches what was saved, not the default
    sound.
17. **Remove the plugin instance** from the mixer insert and confirm FL
    Studio does not crash, hang, or show an error on removal.

## If anything fails

Note the exact step, what was expected vs. what happened, and whether FL
Studio's own crash/error dialog appeared (screenshot if possible) before
reporting back - this checklist exists specifically to catch real-DAW
issues the automated JUCE-hosting tests in
[docs/FULL_DSP_AUDIT.md](FULL_DSP_AUDIT.md) cannot reach.
