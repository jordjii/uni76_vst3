// UNI 76 - production UI bootstrap.
//
// Wires the 7 knob modules to their JUCE parameters through the
// framework's own relay/attachment bridge (juce_webview.js -
// getSliderState) and drives the purely-cosmetic derived indicators
// (aux_visuals.js) off the same value stream. No polling: every update
// here is either a direct user gesture or a valueChangedEvent callback
// fired by the native backend.

import { getSliderState } from "./juce_webview.js";
import { ParameterKnob } from "./knob.js";
import { TiltSlider } from "./tilt.js";
import { bindTriScale, bindPreampFilterLines } from "./aux_visuals.js";
import { initMeters } from "./meters.js";
import { initModulePower } from "./module_power.js";

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

const MODULES = [
  // EQ defaults to its centred/flat "PHONE" position (50%); every other
  // module - including PAN/`panorama` - defaults to fully off (0%) - see
  // ParameterLayout.cpp, which is the source of truth this must stay in
  // sync with.
  { id: "preamp", control: "DRIVE", name: "Preamp", defaultNormalised: 0 },
  { id: "eq", control: "TONE", name: "EQ", defaultNormalised: 0.5 },
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

// IMAGE TILT - the one deliberate exception to "one knob per module"
// (see CLAUDE.md, docs/DSP_IMAGE.md): a second, independent, compact
// control living inside the IMAGE module's own section, bound to its
// own `imageTilt` parameter. Not folded into initModule()/MODULES above,
// which is built around exactly one knob per module.
function initImageTilt() {
  const element = document.querySelector('.tilt[data-param="imageTilt"]');
  if (!element) return;

  const valueElement = document.querySelector(".module__aux--imager .tilt__value");

  new TiltSlider({
    element,
    sliderState: getSliderState("imageTilt"),
    ariaLabel: "Image Tilt",
    valueElement,
    defaultNormalised: 0.5, // -100..100 range, so normalised 0.5 == 0 (CENTER)
  });
}

MODULES.forEach(initModule);
initImageTilt();
initMeters();
initModulePower();
