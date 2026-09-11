// UNI 76 - IMAGE's spatial field pad.
//
// A single square control that drives TWO existing JUCE parameters at
// once through the same WebSliderRelay bridge every other control uses
// (juce_webview.js) - horizontal position is `imageTilt`, vertical
// position is `imager`. No new parameters, no new DSP - this is purely
// an alternative, spatially-intuitive way to set two controls that
// already exist (see app.js and docs/DSP_IMAGE.md's "UI" section).
//
// Sync with the main IMAGE knob (bound to the same `imager` parameter,
// see knob.js/app.js) comes for free: getSliderState(name) returns one
// singleton SliderState per parameter name (juce_webview.js), so both
// widgets share the exact same object and both receive its
// valueChangedEvent whenever the parameter changes from *any* source -
// this pad, the knob, or host automation.

const STEP_NORMAL = 0.01;
const STEP_FINE = 0.001;

function clamp01(value) {
  return Math.min(1, Math.max(0, value));
}

function formatTilt(scaled) {
  const rounded = Math.round(scaled);
  if (rounded === 0) return "CENTER";
  return rounded < 0 ? `L${Math.abs(rounded)}` : `R${rounded}`;
}

export class FieldPad {
  constructor({
    element, xState, yState, ariaLabel = "Image field",
    xDefaultNormalised = 0.5, yDefaultNormalised = 0,
  }) {
    this.element = element;
    this.xState = xState; // imageTilt: 0=LEFT(-100) .. 1=RIGHT(+100)
    // imager is bipolar (-100..+100, live-testing follow-up round - see
    // docs/DSP_IMAGE.md's "Bipolar redesign" section): normalised 0=-100%
    // (MONO), 1=+100% (STEREO), 0.5=0%/CENTER. Still visually inverted
    // (top=1/highest) - this mapping itself has NOT been re-interpreted
    // for the new bipolar range (a separate, deliberate decision not made
    // this round; the pad stays hidden either way - see app.js's
    // initImageField()).
    this.yState = yState;
    this.ariaLabel = ariaLabel;
    this.xDefaultNormalised = xDefaultNormalised;
    this.yDefaultNormalised = yDefaultNormalised;

    this.dragging = false;
    this.keyGestureActive = false;

    this._onPointerDown = this._onPointerDown.bind(this);
    this._onPointerMove = this._onPointerMove.bind(this);
    this._onPointerUp = this._onPointerUp.bind(this);
    this._onDoubleClick = this._onDoubleClick.bind(this);
    this._onKeyDown = this._onKeyDown.bind(this);
    this._onKeyUp = this._onKeyUp.bind(this);
    this._render = this._render.bind(this);

    this._setupAria();
    this._bindEvents();

    // Deliberately do NOT call _render() here - same reasoning as
    // ParameterKnob/TiltSlider: both states start out with placeholder
    // values until the native side answers the initial update, and the
    // HTML-authored resting position (index.html's inline --pad-x/--pad-y)
    // is already correct for the shared default (imager=0, imageTilt=0).
    this.xState.valueChangedEvent.addListener(this._render);
    this.yState.valueChangedEvent.addListener(this._render);
  }

  _setupAria() {
    this.element.setAttribute("role", "group");
    this.element.setAttribute("aria-roledescription", "2D pad");
    this.element.setAttribute("aria-label", `${this.ariaLabel}: Tilt CENTER, Image 0%`);
  }

  _bindEvents() {
    this.element.addEventListener("pointerdown", this._onPointerDown);
    this.element.addEventListener("dblclick", this._onDoubleClick);
    this.element.addEventListener("keydown", this._onKeyDown);
    this.element.addEventListener("keyup", this._onKeyUp);
  }

  _updateFromPointer(event) {
    const rect = this.element.getBoundingClientRect();
    const xFraction = clamp01((event.clientX - rect.left) / rect.width);
    const yFraction = clamp01((event.clientY - rect.top) / rect.height); // 0 = top edge

    this.xState.setNormalisedValue(xFraction);
    this.yState.setNormalisedValue(1 - yFraction);
  }

  _onPointerDown(event) {
    event.preventDefault();
    this.element.focus();
    this.element.setPointerCapture(event.pointerId);

    this.dragging = true;
    this.element.classList.add("field__pad--dragging");

    this.xState.sliderDragStarted();
    this.yState.sliderDragStarted();
    this._updateFromPointer(event);

    this.element.addEventListener("pointermove", this._onPointerMove);
    this.element.addEventListener("pointerup", this._onPointerUp);
    this.element.addEventListener("pointercancel", this._onPointerUp);
  }

  _onPointerMove(event) {
    if (!this.dragging) return;
    this._updateFromPointer(event);
  }

  _onPointerUp(event) {
    if (!this.dragging) return;

    this.dragging = false;
    this.element.classList.remove("field__pad--dragging");

    if (this.element.hasPointerCapture(event.pointerId))
      this.element.releasePointerCapture(event.pointerId);

    this.element.removeEventListener("pointermove", this._onPointerMove);
    this.element.removeEventListener("pointerup", this._onPointerUp);
    this.element.removeEventListener("pointercancel", this._onPointerUp);

    this.xState.sliderDragEnded();
    this.yState.sliderDragEnded();
  }

  _onDoubleClick() {
    this.xState.sliderDragStarted();
    this.yState.sliderDragStarted();
    this.xState.setNormalisedValue(this.xDefaultNormalised);
    this.yState.setNormalisedValue(this.yDefaultNormalised);
    this.xState.sliderDragEnded();
    this.yState.sliderDragEnded();
  }

  _onKeyDown(event) {
    if (event.key === "Home") {
      event.preventDefault();
      this._onDoubleClick();
      return;
    }

    let dx = 0, dy = 0;
    switch (event.key) {
      case "ArrowRight": dx = 1; break;
      case "ArrowLeft": dx = -1; break;
      case "ArrowUp": dy = 1; break;
      case "ArrowDown": dy = -1; break;
      default: return;
    }

    event.preventDefault();

    if (!this.keyGestureActive) {
      this.keyGestureActive = true;
      this.xState.sliderDragStarted();
      this.yState.sliderDragStarted();
    }

    const step = event.shiftKey ? STEP_FINE : STEP_NORMAL;
    if (dx !== 0) this.xState.setNormalisedValue(clamp01(this.xState.getNormalisedValue() + dx * step));
    if (dy !== 0) this.yState.setNormalisedValue(clamp01(this.yState.getNormalisedValue() + dy * step));
  }

  _onKeyUp(event) {
    if (!["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"].includes(event.key)) return;

    if (this.keyGestureActive) {
      this.keyGestureActive = false;
      this.xState.sliderDragEnded();
      this.yState.sliderDragEnded();
    }
  }

  _render() {
    const xNorm = this.xState.getNormalisedValue();
    const yNorm = this.yState.getNormalisedValue();

    // Inset the puck's travel range a little so it always stays fully
    // inside the pad's own border, even at the extremes (0/1) - without
    // this a puck sitting exactly on an edge or corner visually
    // overhangs the border by half its own radius.
    const inset = 7; // percent
    const range = 100 - inset * 2;
    this.element.style.setProperty("--pad-x", `${inset + xNorm * range}%`);
    this.element.style.setProperty("--pad-y", `${inset + (1 - yNorm) * range}%`);

    const tiltText = formatTilt(this.xState.getScaledValue());
    const imagerScaled = Math.round(this.yState.getScaledValue());
    this.element.setAttribute("aria-label", `${this.ariaLabel}: Tilt ${tiltText}, Image ${imagerScaled}%`);
  }
}
