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
import { bindTriScale, bindPreampFilterLines } from "./aux_visuals.js";
import { initMeters } from "./meters.js";
import { initModulePower } from "./module_power.js";

// One full octave down/up, matching the "-OCT ... 0 ... +OCT" scale label
// printed under the PITCH knob - display only, the underlying parameter is
// still the same plain 0..100% APVTS float as every other module.
const PITCH_SEMITONE_RANGE = 12;

function formatPitchSemitones(scaled) {
  const semitones = Math.round((scaled / 100) * (PITCH_SEMITONE_RANGE * 2) - PITCH_SEMITONE_RANGE);
  return semitones === 0 ? "0 ST" : `${semitones > 0 ? "+" : ""}${semitones} ST`;
}

const MODULES = [
  // EQ defaults to its centred/flat "PHONE" position (50%); every other
  // module defaults to fully off (0%) - see ParameterLayout.cpp, which is
  // the source of truth this must stay in sync with.
  { id: "preamp", control: "DRIVE", name: "Preamp", defaultNormalised: 0 },
  { id: "eq", control: "TONE", name: "EQ", defaultNormalised: 0.5 },
  { id: "saturation", control: "HEAT", name: "Saturation", defaultNormalised: 0 },
  { id: "pitch", control: "SHIFT", name: "Pitch", defaultNormalised: 0, formatValue: formatPitchSemitones },
  { id: "panorama", control: "WIDTH", name: "Panorama", defaultNormalised: 0 },
  { id: "reverb", control: "SPACE", name: "Reverb", defaultNormalised: 0 },
  { id: "imager", control: "IMAGE", name: "Imager", defaultNormalised: 0 },
];

function initModule({ id, control, name, defaultNormalised, formatValue }) {
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
    onChange: (_normalised, scaled) => {
      if (auxUpdate) auxUpdate(scaled);
    },
  });
}

MODULES.forEach(initModule);
initMeters();
initModulePower();
