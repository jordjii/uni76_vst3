// UNI 76 - CSS/DOM knob control bound to a single JUCE parameter.
//
// Owns interaction only (drag, wheel, keyboard, double-click, ARIA state);
// all rotation/visual rendering is CSS driven off the --knob-angle custom
// property this class sets. Parameter sync goes exclusively through the
// JUCE WebSliderRelay bridge (juce_webview.js) - no polling, no direct DOM
// reads of "the value" from anywhere else.

const ROTATION_MIN_DEG = -135;
const ROTATION_MAX_DEG = 135;
const ROTATION_RANGE_DEG = ROTATION_MAX_DEG - ROTATION_MIN_DEG;

const STEP_NORMAL = 0.01; // 1% per wheel notch / arrow key press
const STEP_FINE = 0.001; // 0.1% with Shift held

const DRAG_PIXELS_FOR_FULL_RANGE = 220;
const DRAG_PIXELS_FOR_FULL_RANGE_FINE = DRAG_PIXELS_FOR_FULL_RANGE * 8;

const WHEEL_GESTURE_IDLE_MS = 250;

function clamp01(value) {
  return Math.min(1, Math.max(0, value));
}

export class ParameterKnob {
  constructor({ element, sliderState, ariaLabel, valueElement, onChange }) {
    this.element = element;
    this.state = sliderState;
    this.valueElement = valueElement;
    this.onChange = onChange;

    this.dragging = false;
    this.dragStartY = 0;
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

    this._buildTicks();
    this._setupAria(ariaLabel);
    this._bindEvents();

    // Deliberately do NOT call _render() here: this.state starts out with
    // placeholder properties (range 0..1, value 0) until the native side
    // answers the "requestInitialUpdate" event SliderState fires on
    // construction. Rendering that placeholder would flash the knob to 0%
    // for a frame. The HTML/CSS-authored resting state (50%, 0deg) is
    // already the correct default, so we just wait for the first real
    // valueChangedEvent instead.
    this.state.valueChangedEvent.addListener(this._render);
  }

  _buildTicks() {
    const container = this.element.querySelector(".knob__ticks");
    if (!container) return;

    const tickCount = 11; // every 10%, including both ends

    for (let i = 0; i < tickCount; i++) {
      const fraction = i / (tickCount - 1);
      const angle = ROTATION_MIN_DEG + fraction * ROTATION_RANGE_DEG;
      const isMajor = i === 0 || i === (tickCount - 1) / 2 || i === tickCount - 1;

      const tick = document.createElement("span");
      tick.className = isMajor ? "knob__tick knob__tick--major" : "knob__tick";
      tick.style.setProperty("--tick-angle", `${angle}deg`);

      const mark = document.createElement("span");
      mark.className = "knob__tick-mark";
      tick.appendChild(mark);

      container.appendChild(tick);
    }
  }

  _setupAria(ariaLabel) {
    this.element.setAttribute("role", "slider");
    this.element.setAttribute("tabindex", "0");
    this.element.setAttribute("aria-orientation", "vertical");
    this.element.setAttribute("aria-valuemin", "0");
    this.element.setAttribute("aria-valuemax", "100");
    // Matches the HTML/CSS-authored resting state until the first real
    // valueChangedEvent arrives - see the comment in the constructor.
    this.element.setAttribute("aria-valuenow", "50");
    this.element.setAttribute("aria-valuetext", "50%");
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
    this.dragStartY = event.clientY;
    this.dragStartNormalised = this.state.getNormalisedValue();
    this.element.classList.add("knob--dragging");

    this.state.sliderDragStarted();

    this.element.addEventListener("pointermove", this._onPointerMove);
    this.element.addEventListener("pointerup", this._onPointerUp);
    this.element.addEventListener("pointercancel", this._onPointerUp);
  }

  _onPointerMove(event) {
    if (!this.dragging) return;

    const deltaY = this.dragStartY - event.clientY; // dragging up increases the value
    const range = event.shiftKey ? DRAG_PIXELS_FOR_FULL_RANGE_FINE : DRAG_PIXELS_FOR_FULL_RANGE;
    const next = clamp01(this.dragStartNormalised + deltaY / range);

    this.state.setNormalisedValue(next);
  }

  _onPointerUp(event) {
    if (!this.dragging) return;

    this.dragging = false;
    this.element.classList.remove("knob--dragging");

    if (this.element.hasPointerCapture(event.pointerId))
      this.element.releasePointerCapture(event.pointerId);

    this.element.removeEventListener("pointermove", this._onPointerMove);
    this.element.removeEventListener("pointerup", this._onPointerUp);
    this.element.removeEventListener("pointercancel", this._onPointerUp);

    this.state.sliderDragEnded();
  }

  _onWheel(event) {
    event.preventDefault();

    const step = event.shiftKey ? STEP_FINE : STEP_NORMAL;
    const direction = event.deltaY < 0 ? 1 : -1;

    if (!this.wheelGestureActive) {
      this.wheelGestureActive = true;
      this.state.sliderDragStarted();
    }

    const next = clamp01(this.state.getNormalisedValue() + step * direction);
    this.state.setNormalisedValue(next);

    clearTimeout(this.wheelIdleTimer);
    this.wheelIdleTimer = setTimeout(() => {
      this.wheelGestureActive = false;
      this.state.sliderDragEnded();
    }, WHEEL_GESTURE_IDLE_MS);
  }

  _onDoubleClick() {
    this.state.sliderDragStarted();
    this.state.setNormalisedValue(0.5);
    this.state.sliderDragEnded();
  }

  _onKeyDown(event) {
    const step = event.shiftKey ? STEP_FINE : STEP_NORMAL;
    let delta = 0;

    switch (event.key) {
      case "ArrowUp":
      case "ArrowRight":
        delta = step;
        break;
      case "ArrowDown":
      case "ArrowLeft":
        delta = -step;
        break;
      default:
        return;
    }

    event.preventDefault();

    if (!this.keyGestureActive) {
      this.keyGestureActive = true;
      this.state.sliderDragStarted();
    }

    const next = clamp01(this.state.getNormalisedValue() + delta);
    this.state.setNormalisedValue(next);
  }

  _onKeyUp(event) {
    if (!["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight"].includes(event.key)) return;

    if (this.keyGestureActive) {
      this.keyGestureActive = false;
      this.state.sliderDragEnded();
    }
  }

  _render() {
    const normalised = this.state.getNormalisedValue();
    const scaled = Math.round(this.state.getScaledValue());
    const angle = ROTATION_MIN_DEG + normalised * ROTATION_RANGE_DEG;

    this.element.style.setProperty("--knob-angle", `${angle}deg`);
    this.element.setAttribute("aria-valuenow", String(scaled));
    this.element.setAttribute("aria-valuetext", `${scaled}%`);

    if (this.valueElement) this.valueElement.textContent = `${scaled}%`;
    if (this.onChange) this.onChange(normalised, scaled);
  }
}
