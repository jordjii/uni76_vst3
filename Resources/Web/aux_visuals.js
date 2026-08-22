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

/**
 * Binds the Preamp module's two filter-position indicator lines. The
 * mapping below is illustrative only (Low Cut tracks up, High Cut tracks
 * down, symmetrically around the 50% default) - it has no bearing on any
 * future DSP implementation.
 */
export function bindPreampFilterLines(root) {
  const lowCutTrack = root.querySelector('[data-role="low-cut"] .filter-line__track');
  const highCutTrack = root.querySelector('[data-role="high-cut"] .filter-line__track');
  const lowCutMark = root.querySelector('[data-role="low-cut"] .filter-line__mark');
  const highCutMark = root.querySelector('[data-role="high-cut"] .filter-line__mark');

  if (lowCutTrack) buildTrackTicks(lowCutTrack);
  if (highCutTrack) buildTrackTicks(highCutTrack);

  return function update(scaledValue) {
    const lowCutPos = clamp(10 + scaledValue * 0.5, 10, 60);
    const highCutPos = clamp(90 - scaledValue * 0.5, 40, 90);

    if (lowCutMark) lowCutMark.style.setProperty("--pos", `${lowCutPos}%`);
    if (highCutMark) highCutMark.style.setProperty("--pos", `${highCutPos}%`);
  };
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}
