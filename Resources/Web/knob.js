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

const defaultFormatValue = (scaled) => `${scaled}%`;

export class ParameterKnob {
  constructor({
    element, sliderState, ariaLabel, valueElement, onChange,
    defaultNormalised = 0.5, formatValue = defaultFormatValue,
    // Discrete mode (PITCH only - see app.js): steps = number of intervals
    // across the full range (24 for -12..+12 semitones = 25 positions).
    // When set, drag/wheel/keyboard snap exactly to 1/steps of the range,
    // Shift fine-control is ignored (no fractional semitones exist), and
    // ARIA reports the real min/max/step instead of a 0..100 percent range.
    steps = null, ariaMin = 0, ariaMax = 100, ariaStep = null,
  }) {
    this.element = element;
    this.state = sliderState;
    this.valueElement = valueElement;
    this.onChange = onChange;
    this.defaultNormalised = defaultNormalised;
    this.formatValue = formatValue;
    this.steps = steps;
    this.ariaMin = ariaMin;
    this.ariaMax = ariaMax;
    this.ariaStep = ariaStep;

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
    // construction. Rendering that placeholder would flash the knob to a
    // wrong position for a frame. The HTML/CSS-authored resting state
    // (see each knob's inline --knob-angle and its knob__value text in
    // index.html, which must match defaultNormalised/the real APVTS
    // default) is already correct, so we just wait for the first real
    // valueChangedEvent instead.
    this.state.valueChangedEvent.addListener(this._render);
  }

  _buildTicks() {
    const container = this.element.querySelector(".knob__ticks");
    if (!container) return;

    // Discrete knobs (PITCH) get one tick per real step (25 for -12..+12);
    // continuous knobs keep the original 11 (every 10%).
    const tickCount = this.steps != null ? this.steps + 1 : 11;

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
    this.element.setAttribute("aria-valuemin", String(this.ariaMin));
    this.element.setAttribute("aria-valuemax", String(this.ariaMax));
    if (this.ariaStep != null) this.element.setAttribute("aria-valuestep", String(this.ariaStep));
    // Matches the HTML/CSS-authored resting state until the first real
    // valueChangedEvent arrives - see the comment in the constructor. For
    // a discrete knob the resting scaled value is real units (e.g.
    // semitones), not a 0..100 percent.
    const restingScaled = this.steps != null
      ? Math.round(this.ariaMin + this.defaultNormalised * (this.ariaMax - this.ariaMin))
      : Math.round(this.defaultNormalised * 100);
    this.element.setAttribute("aria-valuenow", String(restingScaled));
    this.element.setAttribute("aria-valuetext", this.formatValue(restingScaled));
    if (ariaLabel) this.element.setAttribute("aria-label", ariaLabel);
  }

  /** Discrete-mode only: snaps a normalised (0..1) value to the nearest of
      `steps` equal intervals, computed from a step *index* (not repeated
      float addition) so repeated stepping can never drift off-grid. */
  _snapToStep(normalised) {
    return Math.round(normalised * this.steps) / this.steps;
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
    // Discrete knobs have no fractional positions, so Shift fine-control
    // doesn't apply - always use the normal (coarser) drag range.
    const range = this.steps != null || !event.shiftKey ? DRAG_PIXELS_FOR_FULL_RANGE : DRAG_PIXELS_FOR_FULL_RANGE_FINE;
    let next = clamp01(this.dragStartNormalised + deltaY / range);
    if (this.steps != null) next = this._snapToStep(next);

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

    const direction = event.deltaY < 0 ? 1 : -1;

    if (!this.wheelGestureActive) {
      this.wheelGestureActive = true;
      this.state.sliderDragStarted();
    }

    const next = this.steps != null
      ? this._stepFromCurrent(direction)
      : clamp01(this.state.getNormalisedValue() + (event.shiftKey ? STEP_FINE : STEP_NORMAL) * direction);
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
      case "ArrowUp":
      case "ArrowRight":
        direction = 1;
        break;
      case "ArrowDown":
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

    const next = this.steps != null
      ? this._stepFromCurrent(direction)
      : clamp01(this.state.getNormalisedValue() + (event.shiftKey ? STEP_FINE : STEP_NORMAL) * direction);
    this.state.setNormalisedValue(next);
  }

  /** Discrete-mode only: moves exactly one step from the current (assumed
      already-snapped) position, by step *index* so it can't drift. */
  _stepFromCurrent(direction) {
    const currentStep = Math.round(this.state.getNormalisedValue() * this.steps);
    return clamp01((currentStep + direction) / this.steps);
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
    const formatted = this.formatValue(scaled);

    this.element.style.setProperty("--knob-angle", `${angle}deg`);
    this.element.setAttribute("aria-valuenow", String(scaled));
    this.element.setAttribute("aria-valuetext", formatted);

    if (this.valueElement) this.valueElement.textContent = formatted;
    if (this.onChange) this.onChange(normalised, scaled);
  }
}
