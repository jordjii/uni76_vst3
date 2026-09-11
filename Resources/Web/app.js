// UNI 76 - production UI bootstrap.
//
// Wires the 7 knob modules to their JUCE parameters through the
// framework's own relay/attachment bridge (juce_webview.js -
// getSliderState) and drives the purely-cosmetic derived indicators
// (aux_visuals.js) off the same value stream. No polling: every update
// here is either a direct user gesture or a valueChangedEvent callback
// fired by the native backend.

import { getSliderState, getNativeFunction } from "./juce_webview.js";
import { ParameterKnob } from "./knob.js";
import { FieldPad } from "./field_pad.js";
import { bindTriScale, bindPreampFilterLines } from "./aux_visuals.js";
import { initMeters } from "./meters.js";
import { initModulePower } from "./module_power.js";
import { initChainOrder } from "./chain_order.js";
import { initPresetMenu, initABToggle } from "./header_controls.js";

// PITCH is a discrete -12..+12 semitone APVTS int parameter (25 positions,
// step 1) - unlike every other module's plain 0..100% float. `scaled`
// already arrives as the real semitone value via the JUCE bridge, so this
// is just presentation, not unit conversion.
const PITCH_SEMITONE_RANGE = 12;
const PITCH_STEPS = PITCH_SEMITONE_RANGE * 2; // 24 intervals = 25 positions

function formatPitchSemitones(scaled) {
  const semitones = Math.round(scaled);
  return semitones === 0 ? "0 ST" : `${semitones > 0 ? "+" : ""}${semitones} ST`;
}

// EQ's underlying APVTS parameter is still a plain 0..100% float (default
// 50%, unchanged - see ParameterLayout.cpp) - only the *displayed* value
// is remapped to a symmetric -50%..+50% scale (see docs/DSP_EQ.md), so the
// panel reads as a centred control even though nothing about the
// parameter's own range/type/default changed. `scaled` arrives as the
// real 0..100 value via the JUCE bridge (same as every other 0..100%
// module) - this is presentation only, not a second unit conversion.
function formatEqPercent(scaled) {
  const displayed = Math.round(scaled - 50);
  return displayed === 0 ? "0%" : `${displayed > 0 ? "+" : ""}${displayed}%`;
}

const MODULES = [
  // EQ defaults to its centred/flat "PHONE" position (50%); every other
  // module - including PAN/`panorama` - defaults to fully off (0%) - see
  // ParameterLayout.cpp, which is the source of truth this must stay in
  // sync with.
  { id: "preamp", control: "DRIVE", name: "Preamp", defaultNormalised: 0 },
  { id: "eq", control: "PHONE TONE", name: "EQ", defaultNormalised: 0.5, formatValue: formatEqPercent },
  { id: "saturation", control: "HEAT", name: "Saturation", defaultNormalised: 0 },
  // Discrete: 25 fixed integer semitone positions (-12..+12), default 0 ST
  // (dead centre) - not a percentage like every other module. See
  // Source/Parameters/ParameterLayout.cpp, the source of truth this must
  // stay in sync with.
  {
    id: "pitch", control: "SHIFT", name: "Pitch", defaultNormalised: 0.5, formatValue: formatPitchSemitones,
    discrete: { steps: PITCH_STEPS, ariaMin: -PITCH_SEMITONE_RANGE, ariaMax: PITCH_SEMITONE_RANGE, ariaStep: 1 },
  },
  // PAN is a stereo width + slow ear-to-ear motion control (ORIGINAL /
  // WIDE / MOTION), not an L/R balance pan, despite the `panorama`
  // parameter ID (kept only for compatibility). 0% (ORIGINAL) is the
  // identity/neutral position, so unlike EQ it defaults fully off, same
  // as every other non-EQ module. See Source/Parameters/ParameterLayout.cpp,
  // the source of truth this must stay in sync with, and docs/DSP_PAN.md.
  { id: "panorama", control: "WIDTH", name: "Panorama", defaultNormalised: 0 },
  { id: "reverb", control: "SPACE", name: "Reverb", defaultNormalised: 0 },
  { id: "imager", control: "IMAGE", name: "Imager", defaultNormalised: 0 },
];

function initModule({ id, control, name, defaultNormalised, formatValue, discrete }) {
  const section = document.querySelector(`.module[data-param="${id}"]`);
  if (!section) return;

  const knobElement = section.querySelector(".knob");
  const valueElement = section.querySelector(".knob__value");
  if (!knobElement) return;

  let auxUpdate = null;

  const triScale = section.querySelector(".tri-scale");
  if (triScale) auxUpdate = bindTriScale(triScale);

  const filterLines = section.querySelector(".filter-lines");
  if (filterLines) auxUpdate = bindPreampFilterLines(filterLines);

  new ParameterKnob({
    element: knobElement,
    sliderState: getSliderState(id),
    ariaLabel: `${name} ${control}`,
    valueElement,
    defaultNormalised,
    formatValue,
    steps: discrete ? discrete.steps : null,
    ariaMin: discrete ? discrete.ariaMin : 0,
    ariaMax: discrete ? discrete.ariaMax : 100,
    ariaStep: discrete ? discrete.ariaStep : null,
    onChange: (normalised, _scaled) => {
      // Always feed derived (purely visual) indicators a 0..100 percent
      // position, not the module's own real units - for every module
      // except PITCH those are numerically identical anyway (scaled ===
      // normalised*100 on a plain 0..100% linear parameter).
      if (auxUpdate) auxUpdate(normalised * 100);
    },
  });
}

// IMAGE's spatial field pad - the one deliberate exception to "one knob
// per module" (see CLAUDE.md, docs/DSP_IMAGE.md): a second, independent
// control living inside the IMAGE module's own section that drives BOTH
// `imager` and `imageTilt` at once. Not folded into initModule()/MODULES
// above, which is built around exactly one control per module. Shares
// `imager`'s SliderState singleton with the main IMAGE knob (both call
// getSliderState("imager") and get the exact same object back - see
// field_pad.js), so the two controls stay in sync automatically with no
// extra wiring: whichever one changes the parameter, both receive the
// resulting valueChangedEvent.
// PAN's nested RATE knob (9th public parameter, panRate - live-testing
// follow-up round) - a second, genuinely independent, interactive knob
// physically nested inside the WIDTH knob (knobs.css's --knob-size-inner),
// the same "one deliberate exception" pattern IMAGE's FIELD pad already
// established (see CLAUDE.md's HTML/CSS/JS UI rule). Drives the motion
// LFO's own speed - referencing SoundToys PanMan's own Rate knob - not
// folded into initModule()/MODULES above, which is built around exactly
// one control per module. See docs/DSP_PAN.md's "Motion rate" section.
//
// panRateHz() below is a presentation-only duplicate of
// PanoramaCurves.h's own curve (same reasoning as formatEqPercent above -
// no shared source across the JS/C++ boundary) so the readout shows the
// real, audible speed rather than a bare percentage.
const PAN_RATE_MIN_HZ = 0.05;
const PAN_RATE_MAX_HZ = 8.0;

function formatPanRateHz(scaled) {
  const t = scaled / 100;
  const hz = PAN_RATE_MIN_HZ * Math.pow(PAN_RATE_MAX_HZ / PAN_RATE_MIN_HZ, t);
  return `${hz.toFixed(2)} Hz`;
}

function initPanRateKnob() {
  const section = document.querySelector('.module[data-param="panorama"]');
  if (!section) return;

  const knobElement = section.querySelector(".knob--inner");
  const valueElement = section.querySelector(".knob__value--inner");
  if (!knobElement) return;

  new ParameterKnob({
    element: knobElement,
    sliderState: getSliderState("panRate"),
    ariaLabel: "Panorama Rate",
    valueElement,
    // Matches ParameterLayout.cpp's own default (35.303%) - the exact
    // position that reproduces PAN's original fixed ~0.3Hz LFO speed, so
    // a freshly opened instance's inner knob starts pointing at the same
    // place its parameter default already is, not a generic 0/50%.
    defaultNormalised: 0.35303,
    formatValue: formatPanRateHz,
  });
}

function initImageField() {
  const element = document.querySelector(".field__pad");
  if (!element) return;

  new FieldPad({
    element,
    xState: getSliderState("imageTilt"),
    yState: getSliderState("imager"),
    ariaLabel: "Image field",
    xDefaultNormalised: 0.5, // imageTilt: -100..100, so 0.5 == 0 (CENTER)
    yDefaultNormalised: 0,   // imager: 0..100%, so 0 == 0%
  });
}

// A real bug found by live testing in Ableton: after clicking any button
// in the UI (a module's power LED, PRESET, A/B), that button keeps DOM
// focus - and Space is the HTML default "activate the focused button".
// So pressing Space for the DAW's transport instead silently toggled
// whichever module had been clicked last. Preventing the default action
// of `mousedown` stops the browser giving focus to a button on a *mouse*
// click, without touching Tab-based keyboard navigation (which still
// focuses buttons normally, and where Space-to-activate is correct and
// expected). Delegated from the document in the capture phase so it also
// covers the preset menu's own dynamically-created rows.
function preventButtonFocusStealing() {
  document.addEventListener(
    "mousedown",
    (event) => {
      const target = event.target instanceof Element ? event.target.closest("button") : null;
      if (target) event.preventDefault();
    },
    true
  );
}

MODULES.forEach(initModule);
initPanRateKnob();
initImageField();
initMeters();
initModulePower();
initChainOrder();
initPresetMenu();
initABToggle();
preventButtonFocusStealing();

// Startup profiling only - see docs/FULL_DSP_AUDIT.md's GUI-startup
// measurements. Cheap (one native call, a handful of numbers) and left
// in permanently since it costs nothing at runtime and is the only
// reliable way to catch a future startup-time regression without
// re-instrumenting from scratch.
getNativeFunction ("uni76ReportStartupTiming") (
  window.__uni76T0 || 0,
  window.__uni76DomContentLoadedAt || 0,
  performance.now()
);
