// UNI 76 - compact horizontal bipolar control for IMAGE TILT.
//
// Same interaction contract as ParameterKnob (knob.js) - drag, wheel,
// double-click, keyboard, ARIA - bound through the same JUCE
// WebSliderRelay bridge (juce_webview.js) - but horizontal, not
// rotational, and rendered as a thumb position (--tilt-pos) rather than
// a rotation angle. Kept as its own small class instead of overloading
// ParameterKnob, which is inherently rotational (ticks, --knob-angle,
// vertical drag) - see docs/DSP_IMAGE.md's "UI" section for why TILT is
// visually and architecturally distinct from the 7 main knobs.

const STEP_NORMAL = 0.01; // 1% of the -100..100 range per wheel notch / arrow key press
const STEP_FINE = 0.001; // 0.1% with Shift held

const DRAG_PIXELS_FOR_FULL_RANGE = 220;
const DRAG_PIXELS_FOR_FULL_RANGE_FINE = DRAG_PIXELS_FOR_FULL_RANGE * 8;

const WHEEL_GESTURE_IDLE_MS = 250;

function clamp01(value) {
  return Math.min(1, Math.max(0, value));
}

// scaled arrives as the real -100..100 value (see ParameterLayout.cpp's
// makeImageTiltParameter) - format as the "L 100 <- 0 -> R 100"-style
// industrial convention the product brief asks for, without literally
// reproducing an arrow-glyph string in the value read-out.
function formatTilt(scaled) {
  const rounded = Math.round(scaled);
  if (rounded === 0) return "CENTER";
  return rounded < 0 ? `L ${Math.abs(rounded)}` : `R ${rounded}`;
}

export class TiltSlider {
  constructor({ element, sliderState, ariaLabel, valueElement, onChange, defaultNormalised = 0.5 }) {
    this.element = element;
    this.state = sliderState;
    this.valueElement = valueElement;
    this.onChange = onChange;
    this.defaultNormalised = defaultNormalised;

    this.dragging = false;
    this.dragStartX = 0;
    this.dragStartNormalised = 0.5;

    this.keyGestureActive = false;
    this.wheelGestureActive = false;
    this.wheelIdleTimer = null;

    this._onPointerDown = this._onPointerDown.bind(this);
    this._onPointerMove = this._onPointerMove.bind(this);
    this._onPointerUp = this._onPointerUp.bind(this);
    this._onWheel = this._onWheel.bind(this);
    this._onDoubleClick = this._onDoubleClick.bind(this);
    this._onKeyDown = this._onKeyDown.bind(this);
    this._onKeyUp = this._onKeyUp.bind(this);
    this._render = this._render.bind(this);

    this._setupAria(ariaLabel);
    this._bindEvents();

    // Deliberately do NOT call _render() here - same reasoning as
    // ParameterKnob's constructor: this.state starts out with a
    // placeholder value until the native side answers the initial
    // update, and the HTML/CSS-authored resting state (index.html's
    // inline --tilt-pos: 50% and "CENTER" text) is already correct.
    this.state.valueChangedEvent.addListener(this._render);
  }

  _setupAria(ariaLabel) {
    this.element.setAttribute("role", "slider");
    this.element.setAttribute("tabindex", "0");
    this.element.setAttribute("aria-orientation", "horizontal");
    this.element.setAttribute("aria-valuemin", "-100");
    this.element.setAttribute("aria-valuemax", "100");
    this.element.setAttribute("aria-valuenow", "0");
    this.element.setAttribute("aria-valuetext", "CENTER");
    if (ariaLabel) this.element.setAttribute("aria-label", ariaLabel);
  }

  _bindEvents() {
    this.element.addEventListener("pointerdown", this._onPointerDown);
    this.element.addEventListener("wheel", this._onWheel, { passive: false });
    this.element.addEventListener("dblclick", this._onDoubleClick);
    this.element.addEventListener("keydown", this._onKeyDown);
    this.element.addEventListener("keyup", this._onKeyUp);
  }

  _onPointerDown(event) {
    event.preventDefault();
    this.element.focus();
    this.element.setPointerCapture(event.pointerId);

    this.dragging = true;
    this.dragStartX = event.clientX;
    this.dragStartNormalised = this.state.getNormalisedValue();
    this.element.classList.add("tilt--dragging");

    this.state.sliderDragStarted();

    this.element.addEventListener("pointermove", this._onPointerMove);
    this.element.addEventListener("pointerup", this._onPointerUp);
    this.element.addEventListener("pointercancel", this._onPointerUp);
  }

  _onPointerMove(event) {
    if (!this.dragging) return;

    const deltaX = event.clientX - this.dragStartX; // dragging right increases the value
    const range = event.shiftKey ? DRAG_PIXELS_FOR_FULL_RANGE_FINE : DRAG_PIXELS_FOR_FULL_RANGE;
    const next = clamp01(this.dragStartNormalised + deltaX / range);

    this.state.setNormalisedValue(next);
  }

  _onPointerUp(event) {
    if (!this.dragging) return;

    this.dragging = false;
    this.element.classList.remove("tilt--dragging");

    if (this.element.hasPointerCapture(event.pointerId))
      this.element.releasePointerCapture(event.pointerId);

    this.element.removeEventListener("pointermove", this._onPointerMove);
    this.element.removeEventListener("pointerup", this._onPointerUp);
    this.element.removeEventListener("pointercancel", this._onPointerUp);

    this.state.sliderDragEnded();
  }

  _onWheel(event) {
    event.preventDefault();

    const direction = event.deltaY < 0 ? 1 : -1;

    if (!this.wheelGestureActive) {
      this.wheelGestureActive = true;
      this.state.sliderDragStarted();
    }

    const next = clamp01(this.state.getNormalisedValue() + (event.shiftKey ? STEP_FINE : STEP_NORMAL) * direction);
    this.state.setNormalisedValue(next);

    clearTimeout(this.wheelIdleTimer);
    this.wheelIdleTimer = setTimeout(() => {
      this.wheelGestureActive = false;
      this.state.sliderDragEnded();
    }, WHEEL_GESTURE_IDLE_MS);
  }

  _onDoubleClick() {
    this.state.sliderDragStarted();
    this.state.setNormalisedValue(this.defaultNormalised);
    this.state.sliderDragEnded();
  }

  _onKeyDown(event) {
    let direction = 0;

    switch (event.key) {
      case "ArrowRight":
        direction = 1;
        break;
      case "ArrowLeft":
        direction = -1;
        break;
      default:
        return;
    }

    event.preventDefault();

    if (!this.keyGestureActive) {
      this.keyGestureActive = true;
      this.state.sliderDragStarted();
    }

    const next = clamp01(this.state.getNormalisedValue() + (event.shiftKey ? STEP_FINE : STEP_NORMAL) * direction);
    this.state.setNormalisedValue(next);
  }

  _onKeyUp(event) {
    if (!["ArrowLeft", "ArrowRight"].includes(event.key)) return;

    if (this.keyGestureActive) {
      this.keyGestureActive = false;
      this.state.sliderDragEnded();
    }
  }

  _render() {
    const normalised = this.state.getNormalisedValue();
    const scaled = this.state.getScaledValue();
    const formatted = formatTilt(scaled);

    this.element.style.setProperty("--tilt-pos", `${normalised * 100}%`);
    this.element.setAttribute("aria-valuenow", String(Math.round(scaled)));
    this.element.setAttribute("aria-valuetext", formatted);

    if (this.valueElement) this.valueElement.textContent = formatted;
    if (this.onChange) this.onChange(normalised, scaled);
  }
}
