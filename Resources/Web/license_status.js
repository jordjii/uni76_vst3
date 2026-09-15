// UNI 76 - license status banner.
//
// The native side (Source/UI/WebUIEditor.cpp's timerCallback) piggybacks a
// "licensed" boolean onto the existing 30Hz "meterLevels" event - the same
// event meters.js and header_controls.js already listen to for their own,
// unrelated fields (input/output levels, presetName/presetKind/presetDirty)
// - rather than a dedicated native function or a second timer. This module
// just reacts to it: show the banner only when the plugin is genuinely
// unlicensed (processBlock() is muting all audio in that state - see
// Core/LicenseState.h), hidden otherwise.
export function initLicenseStatus() {
  const banner = document.querySelector(".license-banner");
  if (!banner) return;

  window.__JUCE__.backend.addEventListener("meterLevels", (payload) => {
    banner.hidden = !(payload && payload.licensed === false);
  });
}
