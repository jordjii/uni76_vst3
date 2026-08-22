// UNI 76 - per-module enable/disable toggle.
//
// The enabled/disabled flag is persistent (survives editor close/reopen
// and host state save/reload) but is deliberately NOT an APVTS parameter -
// UNI 76's automation surface is fixed at exactly 7 parameters (see
// CLAUDE.md). The bridge is therefore a small pair of native functions
// (Source/UI/WebUIEditor.cpp) rather than a WebToggleRelay: this module
// fetches the real persisted state once at startup and corrects the
// HTML-authored "all enabled" resting appearance if a loaded project
// actually had a module disabled, then calls the setter whenever the user
// clicks a toggle.

import { getNativeFunction } from "./juce_webview.js";

// Matches Source/Parameters/ParameterIDs.h's ParamID::all order, which is
// also the order Core/ModuleEnableState.h persists the flags in.
const MODULE_ORDER = ["preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager"];

function applyEnabledState(button, module, enabled) {
  module.classList.toggle("is-disabled", !enabled);
  button.setAttribute("aria-pressed", String(enabled));
}

export function initModulePower() {
  const buttons = Array.from(document.querySelectorAll(".module__power"));
  if (buttons.length === 0) return;

  const setModuleEnabled = getNativeFunction("uni76SetModuleEnabled");
  const getModuleEnabledStates = getNativeFunction("uni76GetModuleEnabledStates");

  const entries = buttons
    .map((button) => {
      const module = button.closest(".module");
      const index = module ? MODULE_ORDER.indexOf(module.dataset.param) : -1;
      return { button, module, index };
    })
    .filter((entry) => entry.module && entry.index >= 0);

  entries.forEach(({ button, module, index }) => {
    button.addEventListener("click", () => {
      const nextEnabled = module.classList.contains("is-disabled");
      applyEnabledState(button, module, nextEnabled);
      setModuleEnabled(index, nextEnabled);
    });
  });

  getModuleEnabledStates().then((states) => {
    if (!Array.isArray(states)) return;

    entries.forEach(({ button, module, index }) => {
      if (index < states.length) applyEnabledState(button, module, !!states[index]);
    });
  });
}
