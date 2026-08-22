// UNI 76 - diagnostic UI bridge.
//
// Binds the 7 HTML range controls to the matching JUCE parameters using the
// framework's own relay/attachment mechanism (see Source/UI/WebUIEditor.cpp)
// rather than a hand-rolled polling protocol. juce_webview.js is the JUCE
// framework's own frontend module, vendored locally - no CDN, no network.

import { getSliderState } from "./juce_webview.js";

const paramIds = ["preamp", "eq", "saturation", "pitch", "panorama", "reverb", "imager"];

let firstUpdateReceived = false;

function setBridgeStatus(state, label) {
  const el = document.getElementById("bridge-status");
  if (!el) return;
  el.className = `status status--${state}`;
  el.textContent = `bridge: ${label}`;
}

function bindControl(id) {
  const input = document.getElementById(id);
  const output = document.querySelector(`output[for="${id}"]`);
  const state = getSliderState(id);

  const updateFromBackend = () => {
    const percent = Math.round(state.getScaledValue());
    input.value = String(percent);
    output.textContent = `${percent}%`;

    if (!firstUpdateReceived) {
      firstUpdateReceived = true;
      setBridgeStatus("ok", "connected");
    }
  };

  state.valueChangedEvent.addListener(updateFromBackend);
  updateFromBackend();

  input.addEventListener("pointerdown", () => state.sliderDragStarted());
  input.addEventListener("input", () => {
    output.textContent = `${input.value}%`;
    state.setNormalisedValue(Number(input.value) / 100);
  });
  input.addEventListener("pointerup", () => state.sliderDragEnded());
}

setBridgeStatus("pending", "connecting…");
paramIds.forEach(bindControl);

const versionEl = document.getElementById("build-version");
if (versionEl) {
  const versionData = window.__JUCE__?.initialisationData?.uni76Version;
  const version = Array.isArray(versionData) && versionData.length > 0 ? versionData[0] : "0.0.0";
  versionEl.textContent = `v${version}`;
}

// If no parameter value ever comes back from the backend, this page is
// being viewed outside the plugin host (or the WebView2/WKWebView backend
// failed to initialise) - say so rather than sitting on "connecting...".
setTimeout(() => {
  if (!firstUpdateReceived)
    setBridgeStatus("error", "no host response");
}, 1500);
