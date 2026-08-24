// UNI 76 - RC1 header controls: PRESET dropdown and A/B toggle.
//
// Both bridge through small native functions (Source/UI/WebUIEditor.cpp),
// the same pattern module_power.js already uses for the per-module power
// buttons - not a WebSliderRelay, since neither a preset selection nor
// the A/B slot is an APVTS parameter. Settings (the gear button) has no
// implementation yet for RC1 and stays `disabled` - see
// docs/FULL_DSP_AUDIT.md's RC1 report.

import { getNativeFunction } from "./juce_webview.js";

export function initPresetMenu() {
  const button = document.querySelector('[data-header-control="preset"]');
  if (!button) return;

  const getNames = getNativeFunction("uni76GetFactoryPresetNames");
  const loadPreset = getNativeFunction("uni76LoadFactoryPreset");

  const menu = document.createElement("div");
  menu.className = "preset-menu";
  menu.setAttribute("role", "menu");
  menu.hidden = true;
  button.insertAdjacentElement("afterend", menu);

  let namesPromise = null;
  let open = false;

  function closeMenu() {
    open = false;
    menu.hidden = true;
    button.setAttribute("aria-expanded", "false");
  }

  function openMenu() {
    open = true;
    menu.hidden = false;
    button.setAttribute("aria-expanded", "true");

    if (!namesPromise) {
      namesPromise = getNames().then((names) => {
        menu.innerHTML = "";
        (Array.isArray(names) ? names : []).forEach((name, index) => {
          const item = document.createElement("button");
          item.type = "button";
          item.className = "preset-menu__item";
          item.setAttribute("role", "menuitem");
          item.textContent = name;
          item.addEventListener("click", () => {
            loadPreset(index);
            closeMenu();
          });
          menu.appendChild(item);
        });
      });
    }
  }

  button.setAttribute("aria-haspopup", "true");
  button.setAttribute("aria-expanded", "false");
  button.disabled = false;

  button.addEventListener("click", (event) => {
    event.stopPropagation();
    if (open) closeMenu();
    else openMenu();
  });

  document.addEventListener("click", (event) => {
    if (open && !menu.contains(event.target) && event.target !== button) closeMenu();
  });

  document.addEventListener("keydown", (event) => {
    if (open && event.key === "Escape") closeMenu();
  });
}

export function initABToggle() {
  const button = document.querySelector('[data-header-control="ab"]');
  if (!button) return;

  const toggleAB = getNativeFunction("uni76ToggleAB");
  button.disabled = false;
  button.textContent = "A / B";

  button.addEventListener("click", () => {
    toggleAB().then((active) => {
      if (active === "A" || active === "B") {
        button.textContent = `A / B (${active})`;
        button.setAttribute("aria-pressed", "true");
      }
    });
  });
}
