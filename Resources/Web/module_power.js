// UNI 76 - per-module enable/disable toggle.
//
// This is a front-end-only visual state, deliberately NOT wired to any
// APVTS parameter: UNI 76's parameter surface is fixed at exactly 7
// automatable floats (see CLAUDE.md, "7 immutable public parameters"),
// and adding 7 more bool parameters for this is a real architecture
// decision (new automation lanes, new state-schema entries) that hasn't
// been made yet. Until that decision happens, the power toggle only
// flips a CSS class - it does not persist across editor close/reopen or
// project save/reload, and (since DSP is still passthrough everywhere)
// it has no audio effect either way.

export function initModulePower() {
  const buttons = document.querySelectorAll(".module__power");

  buttons.forEach((button) => {
    button.addEventListener("click", () => {
      const module = button.closest(".module");
      if (!module) return;

      const nowDisabled = module.classList.toggle("is-disabled");
      button.setAttribute("aria-pressed", String(!nowDisabled));
    });
  });
}
