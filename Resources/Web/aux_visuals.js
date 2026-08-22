// UNI 76 - auxiliary/derived visual indicators.
//
// Everything in this file is purely cosmetic: it reads the *already
// existing* value of one of the 7 real parameters (via the callback wired
// up in app.js) and positions a read-out marker from it. None of this
// creates, reads, or writes any additional APVTS parameter, and none of
// it implies DSP behaviour that isn't implemented yet - see the module
// tag text in index.html and CLAUDE.md for the "visual only" framing.
//
// Visually these read as a printed scale on the panel, not a control: a
// thin baseline, a handful of static tick marks, and a small notch marker
// for the current position - never a browser slider's round thumb.

const TRACK_TICK_COUNT = 7;

function buildTrackTicks(track) {
  for (let i = 0; i < TRACK_TICK_COUNT; i++) {
    const position = (i / (TRACK_TICK_COUNT - 1)) * 100;
    const tick = document.createElement("span");
    tick.className = "scale-tick";
    tick.style.left = `${position}%`;
    track.appendChild(tick);
  }
}

/** Binds a generic 3-point scale (DARK-PHONE-AIR style) to a 0..100 value. */
export function bindTriScale(root) {
  const track = root.querySelector(".tri-scale__track");
  const marker = root.querySelector(".tri-scale__marker");
  const labels = Array.from(root.querySelectorAll(".tri-scale__labels span"));

  if (track) buildTrackTicks(track);

  return function update(scaledValue) {
    if (marker) marker.style.setProperty("--marker-pos", `${scaledValue}%`);

    if (labels.length === 3) {
      const activeIndex = scaledValue < 33.3 ? 0 : scaledValue > 66.6 ? 2 : 1;
      labels.forEach((label, index) => label.classList.toggle("is-active", index === activeIndex));
    }
  };
}

// Mirrors Source/DSP/PreampCurves.h::preampLowCutHz / preampHighCutHz
// exactly (see docs/DSP_PREAMP.md for the measured curve/rationale).
// There is no shared runtime between the plugin core and this WebView UI,
// so this is a hand-transcribed literal of the same formula, not a live
// binding - if the DSP curve ever changes, update both places together.
const PREAMP_LOW_CUT_MIN_HZ = 20;
const PREAMP_LOW_CUT_MAX_HZ = 70;
const PREAMP_LOW_CUT_EXPONENT = 1.4;

const PREAMP_HIGH_CUT_MIN_HZ = 11000;
const PREAMP_HIGH_CUT_MAX_HZ = 20000;
const PREAMP_HIGH_CUT_EXPONENT = 1.15;

function preampLowCutHz(t) {
  return PREAMP_LOW_CUT_MIN_HZ + (PREAMP_LOW_CUT_MAX_HZ - PREAMP_LOW_CUT_MIN_HZ) * Math.pow(t, PREAMP_LOW_CUT_EXPONENT);
}

function preampHighCutHz(t) {
  return PREAMP_HIGH_CUT_MAX_HZ - (PREAMP_HIGH_CUT_MAX_HZ - PREAMP_HIGH_CUT_MIN_HZ) * Math.pow(t, PREAMP_HIGH_CUT_EXPONENT);
}

// Log-frequency position on a fixed visual track range - chosen so both
// markers read as clear movement without needing to print an Hz label.
function freqToTrackPosition(freqHz, trackMinHz, trackMaxHz) {
  const t = (Math.log(freqHz) - Math.log(trackMinHz)) / (Math.log(trackMaxHz) - Math.log(trackMinHz));
  return clamp(t * 100, 0, 100);
}

/**
 * Binds the Preamp module's two filter-position indicator lines to the
 * real, drive-dependent Low Cut / High Cut the DSP actually applies (see
 * Source/DSP/PreampProcessor.cpp) - purely a read-out, not a control: it
 * never creates, reads, or writes any additional APVTS parameter.
 */
export function bindPreampFilterLines(root) {
  const lowCutTrack = root.querySelector('[data-role="low-cut"] .filter-line__track');
  const highCutTrack = root.querySelector('[data-role="high-cut"] .filter-line__track');
  const lowCutMark = root.querySelector('[data-role="low-cut"] .filter-line__mark');
  const highCutMark = root.querySelector('[data-role="high-cut"] .filter-line__mark');

  if (lowCutTrack) buildTrackTicks(lowCutTrack);
  if (highCutTrack) buildTrackTicks(highCutTrack);

  return function update(scaledValue) {
    const t = scaledValue / 100;

    // Open (20Hz) reads near the left of the track, restrictive (up to
    // 70Hz) moves right - display range gives headroom above the DSP's
    // actual 70Hz ceiling so the marker never pins to the track's edge.
    const lowCutPos = freqToTrackPosition(preampLowCutHz(t), 20, 100);

    // Open (20kHz) reads near the right of the track, restrictive (down
    // to 11kHz) moves left.
    const highCutPos = freqToTrackPosition(preampHighCutHz(t), 8000, 20000);

    if (lowCutMark) lowCutMark.style.setProperty("--pos", `${lowCutPos}%`);
    if (highCutMark) highCutMark.style.setProperty("--pos", `${highCutPos}%`);
  };
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}
