// UNI 76 - header controls: PRESET menu (factory + user, categorised) and
// the two-letter A/B compare toggle.
//
// Both bridge through small native functions (Source/UI/WebUIEditor.cpp),
// the same pattern module_power.js already uses for the per-module power
// buttons - not a WebSliderRelay, since neither a preset selection nor
// the A/B slot is an APVTS parameter.

import { getNativeFunction } from "./juce_webview.js";
import { refreshModuleEnabledUI } from "./module_power.js";

export function initPresetMenu() {
  const button = document.querySelector('[data-header-control="preset"]');
  const label = button ? button.querySelector(".header__btn-preset-label") : null;
  if (!button || !label) return;

  const getFactoryNames = getNativeFunction("uni76GetFactoryPresetNames");
  const loadFactoryPreset = getNativeFunction("uni76LoadFactoryPreset");
  const getUserNames = getNativeFunction("uni76GetUserPresetNames");
  const userPresetExists = getNativeFunction("uni76UserPresetExists");
  const saveUserPreset = getNativeFunction("uni76SaveUserPreset");
  const loadUserPreset = getNativeFunction("uni76LoadUserPreset");
  const deleteUserPreset = getNativeFunction("uni76DeleteUserPreset");

  const menu = document.createElement("div");
  menu.className = "preset-menu";
  menu.setAttribute("role", "menu");
  menu.hidden = true;
  button.insertAdjacentElement("afterend", menu);

  let open = false;
  let factoryPresetsCache = null; // [{name, category}] - static for the session, safe to cache
  let currentPresetName = null;   // updated by the meterLevels-piggybacked status, see below
  let currentPresetKind = "none"; // "factory" | "user" | "none" - tracked alongside the name so
                                   // stepPreset() below can tell two same-named factory/user
                                   // presets apart and knows where it is in the flat list

  function closeMenu() {
    if (!open) return;
    open = false;
    menu.hidden = true;
    button.setAttribute("aria-expanded", "false");
  }

  function afterAction() {
    closeMenu();
    refreshModuleEnabledUI();
  }

  function buildSection(title) {
    const heading = document.createElement("div");
    heading.className = "preset-menu__section";
    heading.textContent = title;
    heading.setAttribute("role", "presentation");
    menu.appendChild(heading);
  }

  function buildPresetItem(text, onSelect, extra) {
    const row = document.createElement("div");
    row.className = "preset-menu__row";

    const item = document.createElement("button");
    item.type = "button";
    item.className = "preset-menu__item";
    item.setAttribute("role", "menuitem");
    item.textContent = text;
    item.addEventListener("click", onSelect);
    row.appendChild(item);

    if (extra) row.appendChild(extra);
    menu.appendChild(row);
  }

  function buildSaveRow() {
    const row = document.createElement("div");
    row.className = "preset-menu__save-row";

    const input = document.createElement("input");
    input.type = "text";
    input.className = "preset-menu__save-input";
    input.placeholder = "Preset name";
    input.maxLength = 48;

    const save = document.createElement("button");
    save.type = "button";
    save.className = "preset-menu__save-btn";
    save.textContent = "SAVE CURRENT";

    const doSave = () => {
      const name = input.value.trim();
      if (!name) return;

      userPresetExists(name).then((exists) => {
        if (exists && !window.confirm(`Overwrite user preset "${name}"?`)) return;
        saveUserPreset(name).then((ok) => {
          if (ok) {
            label.textContent = name;
            currentPresetName = name;
            currentPresetKind = "user";
            afterAction();
          }
        });
      });
    };

    save.addEventListener("click", doSave);
    input.addEventListener("keydown", (event) => {
      if (event.key === "Enter") doSave();
      event.stopPropagation();
    });

    row.appendChild(input);
    row.appendChild(save);
    menu.appendChild(row);
  }

  function rebuildMenu() {
    menu.innerHTML = "";

    const factoryReady = factoryPresetsCache ? Promise.resolve(factoryPresetsCache) : getFactoryNames();
    Promise.all([factoryReady, getUserNames()]).then(([factoryEntries, userNames]) => {
      factoryPresetsCache = Array.isArray(factoryEntries) ? factoryEntries : [];

      const categories = [];
      factoryPresetsCache.forEach((entry, index) => {
        let bucket = categories.find((c) => c.category === entry.category);
        if (!bucket) {
          bucket = { category: entry.category, items: [] };
          categories.push(bucket);
        }
        bucket.items.push({ name: entry.name, index });
      });

      categories.forEach(({ category, items }) => {
        buildSection(category);
        items.forEach(({ name, index }) => {
          buildPresetItem(name, () => {
            loadFactoryPreset(index);
            label.textContent = name;
            currentPresetName = name;
            currentPresetKind = "factory";
            afterAction();
          });
        });
      });

      buildSection("USER");
      (Array.isArray(userNames) ? userNames : []).forEach((name) => {
        const del = document.createElement("button");
        del.type = "button";
        del.className = "preset-menu__delete-btn";
        del.setAttribute("aria-label", `Delete user preset ${name}`);
        del.textContent = "✕";
        del.addEventListener("click", (event) => {
          event.stopPropagation();
          if (!window.confirm(`Delete user preset "${name}"?`)) return;
          deleteUserPreset(name).then((ok) => {
            if (ok) {
              if (currentPresetName === name) {
                currentPresetName = null;
                currentPresetKind = "none";
                label.textContent = "CUSTOM";
              }
              closeMenu();
            }
          });
        });

        buildPresetItem(name, () => {
          loadUserPreset(name).then((ok) => {
            if (ok) {
              label.textContent = name;
              currentPresetName = name;
              currentPresetKind = "user";
            }
          });
          afterAction();
        }, del);
      });

      buildSaveRow();
    });
  }

  function openMenu() {
    open = true;
    menu.hidden = false;
    button.setAttribute("aria-expanded", "true");
    rebuildMenu();
  }

  button.addEventListener("click", (event) => {
    event.stopPropagation();
    if (open) closeMenu();
    else openMenu();
  });

  // Capture phase, not bubble: a widget elsewhere in the app (a knob drag,
  // the field pad, etc.) may legitimately call stopPropagation() on its own
  // pointer/click handling during the bubble phase - if outside-click
  // detection only listened on bubble, a click on one of those widgets
  // while the menu was open would never reach this listener and the menu
  // would be stuck open. Capture runs before any such stopPropagation can
  // take effect.
  document.addEventListener(
    "click",
    (event) => {
      if (open && !menu.contains(event.target) && event.target !== button && !button.contains(event.target))
        closeMenu();
    },
    true
  );

  document.addEventListener("keydown", (event) => {
    if (open && event.key === "Escape") closeMenu();
  });

  // Active-preset label + dirty marker (item 4) - piggybacked on the
  // existing 30Hz meter event rather than a new poll (see
  // WebUIEditor.cpp's timerCallback()) so this never needs its own timer
  // or an expensive per-parameter-callback check.
  window.__JUCE__.backend.addEventListener("meterLevels", (payload) => {
    if (open) return; // don't rewrite the button label while the user is browsing the menu
    if (!payload || payload.presetKind === "none" || !payload.presetName) {
      if (currentPresetName !== null) {
        currentPresetName = null;
        currentPresetKind = "none";
        label.textContent = "CUSTOM";
      }
      return;
    }

    currentPresetName = payload.presetName;
    currentPresetKind = payload.presetKind;
    label.textContent = payload.presetDirty ? `${payload.presetName} *` : payload.presetName;
  });

  // ---- Prev/next arrows (item: preset browsing without opening the
  // dropdown) - walk one flat list built fresh each step from the same
  // two native calls the dropdown itself uses: every factory preset in
  // FactoryPresets.h's own table order, then every user preset in
  // uni76GetUserPresetNames()'s own order. Rebuilt on every step (not
  // cached) so a preset saved/deleted in another editor instance, or a
  // moment ago in this one, is always reflected - the list is small and
  // this is a deliberate user click, not a hot path.
  const prevButton = document.querySelector('[data-header-control="preset-prev"]');
  const nextButton = document.querySelector('[data-header-control="preset-next"]');

  function buildFlatList() {
    const factoryReady = factoryPresetsCache ? Promise.resolve(factoryPresetsCache) : getFactoryNames();
    return Promise.all([factoryReady, getUserNames()]).then(([factoryEntries, userNames]) => {
      factoryPresetsCache = Array.isArray(factoryEntries) ? factoryEntries : [];
      const list = factoryPresetsCache.map((entry, index) => ({ kind: "factory", name: entry.name, index }));
      (Array.isArray(userNames) ? userNames : []).forEach((name) => list.push({ kind: "user", name }));
      return list;
    });
  }

  function applyFlatItem(item) {
    if (item.kind === "factory") {
      loadFactoryPreset(item.index);
      label.textContent = item.name;
      currentPresetName = item.name;
      currentPresetKind = "factory";
      refreshModuleEnabledUI();
    } else {
      loadUserPreset(item.name).then((ok) => {
        if (ok) {
          label.textContent = item.name;
          currentPresetName = item.name;
          currentPresetKind = "user";
        }
        refreshModuleEnabledUI();
      });
    }
  }

  function stepPreset(direction) {
    closeMenu();
    buildFlatList().then((list) => {
      if (list.length === 0) return;

      let index = list.findIndex((item) => item.kind === currentPresetKind && item.name === currentPresetName);
      // Not currently on a known preset (CUSTOM, or a preset that no
      // longer exists) - Next starts at the first entry, Prev at the
      // last, rather than requiring two clicks to "catch up".
      if (index === -1)
        index = direction > 0 ? -1 : 0;

      const next = (index + direction + list.length) % list.length;
      applyFlatItem(list[next]);
    });
  }

  if (prevButton) prevButton.addEventListener("click", () => stepPreset(-1));
  if (nextButton) nextButton.addEventListener("click", () => stepPreset(1));
}

export function initABToggle() {
  const group = document.querySelector('[data-header-control="ab"]');
  if (!group) return;

  const buttonA = group.querySelector('[data-ab-slot="a"]');
  const buttonB = group.querySelector('[data-ab-slot="b"]');
  if (!buttonA || !buttonB) return;

  const toggleAB = getNativeFunction("uni76ToggleAB");
  let active = "A";

  function setActive(next) {
    active = next;
    buttonA.classList.toggle("is-active", active === "A");
    buttonB.classList.toggle("is-active", active === "B");
    buttonA.setAttribute("aria-pressed", String(active === "A"));
    buttonB.setAttribute("aria-pressed", String(active === "B"));
  }

  function handleClick(target) {
    if (target === active) return; // already on this slot - no-op, no popup, no sound jump
    toggleAB().then((result) => {
      if (result === "A" || result === "B") {
        setActive(result);
        refreshModuleEnabledUI();
      }
    });
  }

  buttonA.addEventListener("click", () => handleClick("A"));
  buttonB.addEventListener("click", () => handleClick("B"));
}
