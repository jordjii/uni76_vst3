// UNI 76 - INPUT/OUTPUT meter telemetry.
//
// Drives the segmented meters from real signal data only. The native side
// (Source/UI/WebUIEditor.cpp) reads the processor's lock-free LevelMeters
// on a 30 Hz message-thread timer, applies attack/release smoothing, and
// emits a "meterLevels" JUCE backend event with { input, output } linear
// amplitude values (0..1, occasionally slightly over on true clipping).
// This module never guesses, animates, or polls - it only ever reacts to
// that event. With no signal it never receives anything above 0, so the
// meters stay fully grey, matching the native "silence" idle state.

// Keep in sync with the segment-count comment in tokens.css.
const SEGMENT_COUNT = 18;
const MIN_DB = -24;
const MAX_DB = 3;

function zoneForDb(db) {
  if (db >= 0) return "red";
  if (db >= -3) return "orange";
  if (db >= -12) return "yellow";
  return "green";
}

function buildSegments(container) {
  const segments = [];

  for (let i = 0; i < SEGMENT_COUNT; i++) {
    // Colour each segment by the dB value at its outer (loudest) edge, so
    // a segment lights in the colour of the level it represents.
    const segmentDb = MIN_DB + ((i + 1) / SEGMENT_COUNT) * (MAX_DB - MIN_DB);

    const segment = document.createElement("span");
    segment.className = "meter-bar__segment";
    segment.dataset.zone = zoneForDb(segmentDb);
    container.appendChild(segment);
    segments.push(segment);
  }

  return segments;
}

function amplitudeToDb(amplitude) {
  if (!(amplitude > 0)) return -Infinity; // silence, or a non-finite guard
  return 20 * Math.log10(amplitude);
}

function litSegmentCount(db) {
  if (!Number.isFinite(db)) return 0;

  const clamped = Math.min(MAX_DB, Math.max(MIN_DB, db));
  const fraction = (clamped - MIN_DB) / (MAX_DB - MIN_DB);
  return Math.round(fraction * SEGMENT_COUNT);
}

function updateMeter(segments, amplitude) {
  const litCount = litSegmentCount(amplitudeToDb(amplitude));

  for (let i = 0; i < segments.length; i++)
    segments[i].classList.toggle("is-lit", i < litCount);
}

export function initMeters() {
  const inputBar = document.querySelector(".meter--input .meter-bar");
  const outputBar = document.querySelector(".meter--output .meter-bar");
  if (!inputBar || !outputBar) return;

  const inputSegments = buildSegments(inputBar);
  const outputSegments = buildSegments(outputBar);

  // juce_webview.js (imported by app.js before this module runs) guarantees
  // window.__JUCE__.backend exists, real or placeholder.
  window.__JUCE__.backend.addEventListener("meterLevels", (payload) => {
    updateMeter(inputSegments, payload.input);
    updateMeter(outputSegments, payload.output);
  });
}
