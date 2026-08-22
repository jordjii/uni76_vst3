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

const MODULES = [
  { id: "preamp", control: "DRIVE", name: "Preamp" },
  { id: "eq", control: "TONE", name: "EQ" },
  { id: "saturation", control: "HEAT", name: "Saturation" },
  { id: "pitch", control: "SHIFT", name: "Pitch" },
  { id: "panorama", control: "WIDTH", name: "Panorama" },
  { id: "reverb", control: "SPACE", name: "Reverb" },
  { id: "imager", control: "IMAGE", name: "Imager" },
];

function initModule({ id, control, name }) {
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
    onChange: (_normalised, scaled) => {
      if (auxUpdate) auxUpdate(scaled);
    },
  });
}

MODULES.forEach(initModule);
