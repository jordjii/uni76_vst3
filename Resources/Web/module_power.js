// UNI 76 - module enable/disable state: per-strip power buttons.
//
// The enabled/disabled flag is persistent (survives editor close/reopen
// and host state save/reload) but is deliberately NOT an APVTS parameter -
// UNI 76's automation surface is fixed at exactly 8 parameters (see
// CLAUDE.md). The bridge is therefore a small pair of native functions
// (Source/UI/WebUIEditor.cpp) rather than a WebToggleRelay.
//
// The footer's SIGNAL PATH chips (a second view of this same state) were
// removed on request this round (see index.html) - the `.signal-path__chip`
// selector below now simply matches nothing, so `chipEntries` is always
// empty and every chip-related branch here is a harmless no-op. Left in
// place rather than torn out, in case a future round wants that footer
// surface back - `refreshModuleEnabledUI()` would just start driving it
// again with zero other changes needed.
//
// refreshModuleEnabledUI() re-reads the real backend state and redraws
// every registered view from it (never an optimistic DOM-only toggle on
// just the clicked widget) - also called after a preset load and after
// an A/B switch (see header_controls.js), which is what fixes the RC1 bug
// where loading a preset changed the sound but left the power indicators
// visually stale.

import { getNativeFunction } from "./juce_webview.js";

// Matches Source/Parameters/ParameterIDs.h's ParamID::all order, which is
// also the order Core/ModuleEnableState.h persists the flags in. "delay"
// (index 7, added 2026-09-14 - see docs/DSP_DELAY.md) is APPENDED, not
// inserted at its default chain *position* - role index and default chain
// position are independent (see Core/ChainOrder.h's own class comment).
const MODULE_ORDER = ["preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager", "delay"];

let setModuleEnabled = null;
let getModuleEnabledStates = null;
let stripEntries = [];
let chipEntries = [];

function applyToStrip({ button, module }, enabled) {
  module.classList.toggle("is-disabled", !enabled);
  button.setAttribute("aria-pressed", String(enabled));
}

function applyToChip({ chip }, enabled) {
  chip.classList.toggle("is-off", !enabled);
  chip.setAttribute("aria-pressed", String(enabled));
}

function applyStatesLocally(states) {
  stripEntries.forEach((entry) => {
    if (entry.index < states.length) applyToStrip(entry, !!states[entry.index]);
  });
  chipEntries.forEach((entry) => {
    if (entry.index < states.length) applyToChip(entry, !!states[entry.index]);
  });
}

// Re-reads the real backend state and redraws every view from it - the
// single authoritative sync point (see module comment above). Cheap: one
// native round trip, 7 booleans.
export function refreshModuleEnabledUI() {
  if (!getModuleEnabledStates) return Promise.resolve();
  return getModuleEnabledStates().then((states) => {
    if (Array.isArray(states)) applyStatesLocally(states);
  });
}

function setEnabledAndRefresh(index, enabled) {
  setModuleEnabled(index, enabled);
  // Read the state back rather than assuming the setter landed exactly as
  // requested - keeps every view (strip + footer) consistent even if two
  // clicks race each other.
  refreshModuleEnabledUI();
}

export function initModulePower() {
  setModuleEnabled = getNativeFunction("uni76SetModuleEnabled");
  getModuleEnabledStates = getNativeFunction("uni76GetModuleEnabledStates");

  stripEntries = Array.from(document.querySelectorAll(".module__power"))
    .map((button) => {
      const module = button.closest(".module");
      const index = module ? MODULE_ORDER.indexOf(module.dataset.param) : -1;
      return { button, module, index };
    })
    .filter((entry) => entry.module && entry.index >= 0);

  chipEntries = Array.from(document.querySelectorAll(".signal-path__chip"))
    .map((chip) => ({ chip, index: MODULE_ORDER.indexOf(chip.dataset.module) }))
    .filter((entry) => entry.index >= 0);

  stripEntries.forEach((entry) => {
    entry.button.addEventListener("click", () => {
      const nextEnabled = entry.module.classList.contains("is-disabled");
      setEnabledAndRefresh(entry.index, nextEnabled);
    });
  });

  chipEntries.forEach((entry) => {
    entry.chip.addEventListener("click", () => {
      const nextEnabled = entry.chip.classList.contains("is-off");
      setEnabledAndRefresh(entry.index, nextEnabled);
    });
  });

  refreshModuleEnabledUI();
}
