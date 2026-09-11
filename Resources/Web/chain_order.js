// UNI 76 - drag-and-drop pedalboard reordering.
//
// Each module's own knob/aux controls stay exactly where they are; only
// its `.module__header` (name + tag) is a drag handle, so dragging a knob
// never gets mistaken for a reorder. Dropping a module onto another one
// swaps their processing positions - PluginProcessor::processBlock()
// genuinely dispatches through this order (see Core/ChainOrder.h), so
// dragging changes the sound, not just the on-screen layout.
//
// Bridges through two small native functions (uni76GetChainOrder /
// uni76SetChainOrder, see WebUIEditor.cpp), same pattern
// module_power.js's enable flags already use - not a WebSliderRelay,
// since chain order is structural state, not an APVTS parameter.

import { getNativeFunction } from "./juce_webview.js";

// Matches Source/Parameters/ParameterIDs.h's ParamID::all order, which is
// also the order Core/ChainOrder.h's role indices use - kept identical to
// module_power.js's own MODULE_ORDER (duplicated, not imported, so this
// file has no load-order dependency on that one).
const MODULE_ORDER = ["preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager"];

let getChainOrder = null;
let setChainOrder = null;
let modules = [];
let moduleByRole = new Map();

// roleOrder[position] = which role runs at that position - applies it as
// each module's CSS grid `order` (app__main is `display: grid`, so this
// alone repositions the panel; no DOM node ever moves).
function applyPositions(roleOrder) {
  roleOrder.forEach((role, position) => {
    const module = moduleByRole.get(role);
    if (module) module.style.order = String(position);
  });
}

// Reads the *current on-screen* order back out as an array of roles - the
// starting point for computing a new order after a drop. Falls back to
// each module's own written DOM position for any module whose CSS `order`
// was never explicitly set yet (the very first read, before
// uni76GetChainOrder()'s reply has applied anything).
function currentRoleOrder() {
  return modules
    .map((module, domIndex) => ({
      role: MODULE_ORDER.indexOf(module.dataset.param),
      position: module.style.order !== "" ? parseInt(module.style.order, 10) : domIndex,
    }))
    .sort((a, b) => a.position - b.position)
    .map((entry) => entry.role);
}

// Re-reads the real backend chain order and redraws the panel layout from
// it - the same "never assume, always re-read the source of truth" pattern
// module_power.js's refreshModuleEnabledUI() already established. A preset
// (factory or user - see Core/FactoryPresets.h / Core/UserPresets.h) can
// now carry its own saved chain order, and A/B can swap to a slot with a
// different one too, so both need to re-sync the visual layout after the
// fact, not just after a drag gesture.
export function refreshChainOrderUI() {
  if (!getChainOrder || moduleByRole.size !== MODULE_ORDER.length) return Promise.resolve();
  return getChainOrder().then((order) => {
    if (Array.isArray(order) && order.length === MODULE_ORDER.length) applyPositions(order);
  });
}

export function initChainOrder() {
  getChainOrder = getNativeFunction("uni76GetChainOrder");
  setChainOrder = getNativeFunction("uni76SetChainOrder");

  modules = Array.from(document.querySelectorAll(".module"));
  moduleByRole = new Map();
  modules.forEach((module) => {
    const role = MODULE_ORDER.indexOf(module.dataset.param);
    if (role >= 0) moduleByRole.set(role, module);
  });

  if (moduleByRole.size !== MODULE_ORDER.length) return; // markup mismatch - fail safe, no reordering

  // The persisted DSP order is authoritative - sync the visual layout to
  // it on load rather than trusting the document's own written order,
  // which is only ever the factory-default fallback.
  refreshChainOrderUI();

  let draggedModule = null;

  modules.forEach((module) => {
    const header = module.querySelector(".module__header");
    if (!header) return;

    header.addEventListener("dragstart", (event) => {
      draggedModule = module;
      module.classList.add("is-dragging");
      event.dataTransfer.effectAllowed = "move";
      // Some WebView2/browser drag implementations refuse to start a
      // drag at all unless dataTransfer carries something - the reorder
      // logic itself never reads this back.
      event.dataTransfer.setData("text/plain", module.dataset.param || "");
    });

    header.addEventListener("dragend", () => {
      module.classList.remove("is-dragging");
      modules.forEach((m) => m.classList.remove("is-drop-target"));
      draggedModule = null;
    });

    module.addEventListener("dragover", (event) => {
      if (!draggedModule || draggedModule === module) return;
      event.preventDefault(); // required for `drop` to fire at all
      event.dataTransfer.dropEffect = "move";
      module.classList.add("is-drop-target");
    });

    module.addEventListener("dragleave", () => {
      module.classList.remove("is-drop-target");
    });

    module.addEventListener("drop", (event) => {
      event.preventDefault();
      module.classList.remove("is-drop-target");
      if (!draggedModule || draggedModule === module) return;

      const order = currentRoleOrder();
      const draggedRole = MODULE_ORDER.indexOf(draggedModule.dataset.param);
      const targetRole = MODULE_ORDER.indexOf(module.dataset.param);

      const fromIndex = order.indexOf(draggedRole);
      const toIndex = order.indexOf(targetRole);
      if (fromIndex === -1 || toIndex === -1) return;

      // Move the dragged role to sit where the drop target currently is -
      // a plain reinsert, not a two-item swap, so dropping module A onto
      // module C in a longer row shifts B over by one rather than just
      // trading A and C's positions.
      order.splice(fromIndex, 1);
      order.splice(toIndex, 0, draggedRole);

      applyPositions(order);
      setChainOrder(order);
    });
  });
}
