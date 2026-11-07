// @ts-check
// Standalone event targets: no DOM tree, native bindings, or event-loop integration.
// Dispatch is synchronous. Globals are not modified.

/**
 * @typedef {object} EventInit
 * @property {boolean} [bubbles]
 * @property {boolean} [cancelable]
 * @property {boolean} [composed]
 */

/**
 * @typedef {object} EventState
 * @property {string} type
 * @property {boolean} bubbles
 * @property {boolean} cancelable
 * @property {boolean} composed
 * @property {number} timeStamp
 * @property {EventTarget | null} target
 * @property {EventTarget | null} currentTarget
 * @property {number} eventPhase
 * @property {boolean} dispatching
 * @property {boolean} canceled
 * @property {boolean} propagationStopped
 * @property {boolean} immediatePropagationStopped
 * @property {boolean} inPassiveListener
 */

/** @type {WeakMap<Event, EventState>} */
const eventStates = new WeakMap();

/** @type {(error: unknown) => void} */
let eventErrorReporter = (error) => {
  // The embedding host must observe unhandled rejections or install a reporter.
  void Promise.reject(error);
};

/**
 * Host integration hook, not a browser API. Listener exceptions never escape
 * dispatchEvent(). A throwing reporter becomes an unhandled Promise rejection.
 * @param {(error: unknown) => void} reporter
 * @returns {void}
 */
export function setEventErrorReporter(reporter) {
  if (typeof reporter !== "function") {
    throw new TypeError("Event error reporter must be a function");
  }
  eventErrorReporter = reporter;
}

export class Event {
  static get NONE() {
    return 0;
  }

  static get CAPTURING_PHASE() {
    return 1;
  }

  static get AT_TARGET() {
    return 2;
  }

  static get BUBBLING_PHASE() {
    return 3;
  }

  /** @type {EventState} */
  #state;

  /**
   * @param {string} type
   * @param {EventInit | null} [eventInit]
   */
  constructor(type, eventInit) {
    if (arguments.length === 0) {
      throw new TypeError("Event requires a type");
    }
    if (typeof type === "symbol") {
      throw new TypeError("Event type cannot be a Symbol");
    }
    const eventType = String(type);
    if (eventInit !== undefined && eventInit !== null && typeof eventInit !== "object" && typeof eventInit !== "function") {
      throw new TypeError("Event options must be an object");
    }
    const bubbles = Boolean(eventInit?.bubbles);
    const cancelable = Boolean(eventInit?.cancelable);
    const composed = Boolean(eventInit?.composed);
    // QuickJS-NG installs this monotonic clock with JS_NewContext().
    /** @type {{now: () => number}} */
    const performance = Reflect.get(globalThis, "performance");
    this.#state = {
      type: eventType,
      bubbles,
      cancelable,
      composed,
      timeStamp: performance.now(),
      target: null,
      currentTarget: null,
      eventPhase: 0,
      dispatching: false,
      canceled: false,
      propagationStopped: false,
      immediatePropagationStopped: false,
      inPassiveListener: false,
    };
    eventStates.set(this, this.#state);
    /** @readonly */
    this.isTrusted = false;
    Object.defineProperty(this, "isTrusted", {
      value: false,
      writable: false,
      enumerable: true,
      configurable: false,
    });
  }

  get NONE() {
    return 0;
  }

  get CAPTURING_PHASE() {
    return 1;
  }

  get AT_TARGET() {
    return 2;
  }

  get BUBBLING_PHASE() {
    return 3;
  }

  get type() {
    return this.#state.type;
  }

  get target() {
    return this.#state.target;
  }

  get srcElement() {
    return this.#state.target;
  }

  get currentTarget() {
    return this.#state.currentTarget;
  }

  get eventPhase() {
    return this.#state.eventPhase;
  }

  get bubbles() {
    return this.#state.bubbles;
  }

  get cancelable() {
    return this.#state.cancelable;
  }

  get composed() {
    return this.#state.composed;
  }

  get defaultPrevented() {
    return this.#state.canceled;
  }

  get timeStamp() {
    return this.#state.timeStamp;
  }

  get cancelBubble() {
    return this.#state.propagationStopped;
  }

  set cancelBubble(value) {
    if (value) {
      this.#state.propagationStopped = true;
    }
  }

  get returnValue() {
    return !this.#state.canceled;
  }

  set returnValue(value) {
    if (!value && this.#state.cancelable && !this.#state.inPassiveListener) {
      this.#state.canceled = true;
    }
  }

  stopPropagation() {
    this.#state.propagationStopped = true;
  }

  stopImmediatePropagation() {
    this.#state.propagationStopped = true;
    this.#state.immediatePropagationStopped = true;
  }

  preventDefault() {
    if (this.#state.cancelable && !this.#state.inPassiveListener) {
      this.#state.canceled = true;
    }
  }

  /** @returns {Array<EventTarget>} */
  composedPath() {
    if (this.#state.dispatching && this.#state.target !== null) {
      return [this.#state.target];
    }
    return [];
  }

  /**
   * Legacy reinitialization. Does nothing during dispatch.
   * @param {string} type
   * @param {boolean} [bubbles]
   * @param {boolean} [cancelable]
   * @returns {void}
   */
  initEvent(type, bubbles, cancelable) {
    if (arguments.length === 0) {
      throw new TypeError("initEvent requires a type");
    }
    if (typeof type === "symbol") {
      throw new TypeError("Event type cannot be a Symbol");
    }
    const eventType = String(type);
    if (this.#state.dispatching) {
      return;
    }
    this.#state.type = eventType;
    this.#state.bubbles = Boolean(bubbles);
    this.#state.cancelable = Boolean(cancelable);
    this.#state.target = null;
    this.#state.canceled = false;
    this.#state.propagationStopped = false;
    this.#state.immediatePropagationStopped = false;
  }

  get [Symbol.toStringTag]() {
    return "Event";
  }
}

/** @template [Detail=unknown] */
export class CustomEvent extends Event {
  /** @type {Detail | null} */
  #detail = null;

  /**
   * @param {string} type
   * @param {(EventInit & {detail?: Detail}) | null} [eventInit]
   */
  constructor(type, eventInit) {
    if (arguments.length === 0) {
      throw new TypeError("CustomEvent requires a type");
    }
    super(type, eventInit);
    const detail = eventInit?.detail;
    if (detail !== undefined) {
      this.#detail = detail;
    }
  }

  get detail() {
    return this.#detail;
  }

  /**
   * @param {string} type
   * @param {boolean} [bubbles]
   * @param {boolean} [cancelable]
   * @param {Detail | null} [detail]
   * @returns {void}
   */
  initCustomEvent(type, bubbles, cancelable, detail) {
    if (arguments.length === 0) {
      throw new TypeError("initCustomEvent requires a type");
    }
    // initEvent performs argument conversion even when dispatch is in progress.
    super.initEvent(type, bubbles, cancelable);
    const eventState = eventStates.get(this);
    if (eventState === undefined || eventState.dispatching) {
      return;
    }
    this.#detail = null;
    if (detail !== undefined) {
      this.#detail = detail;
    }
  }

  get [Symbol.toStringTag]() {
    return "CustomEvent";
  }
}

/**
 * @typedef {((this: EventTarget, event: Event) => void) | {handleEvent: (event: Event) => void}} EventListener
 */

/**
 * Structural contract for the later AbortSignal module; no import dependency.
 * @typedef {object} EventListenerSignal
 * @property {boolean} aborted
 * @property {(type: string, callback: EventListener, options?: boolean | AddEventListenerOptions) => void} addEventListener
 * @property {(type: string, callback: EventListener, options?: boolean | EventListenerOptions) => void} removeEventListener
 */

/**
 * @typedef {object} EventListenerOptions
 * @property {boolean} [capture]
 */

/**
 * @typedef {object} AddEventListenerOptions
 * @property {boolean} [capture]
 * @property {boolean} [once]
 * @property {boolean} [passive]
 * @property {EventListenerSignal} [signal]
 */

/**
 * @typedef {object} ListenerRegistration
 * @property {string} type
 * @property {EventListener} callback
 * @property {boolean} capture
 * @property {boolean} once
 * @property {boolean} passive
 * @property {boolean} removed
 * @property {(() => void) | null} abortCleanup
 */

export class EventTarget {
  /** @type {Array<ListenerRegistration>} */
  #listeners = [];

  /**
   * @param {string} type
   * @param {EventListener | null} callback
   * @param {boolean | AddEventListenerOptions | null} [options]
   * @returns {void}
   */
  addEventListener(type, callback, options) {
    if (arguments.length < 2) {
      throw new TypeError("addEventListener requires a type and callback");
    }
    if (typeof type === "symbol") {
      throw new TypeError("Event type cannot be a Symbol");
    }
    const eventType = String(type);
    if (callback !== null && callback !== undefined && typeof callback !== "function" && typeof callback !== "object") {
      throw new TypeError("Event listener must be a function or object");
    }
    let capture = false;
    let once = false;
    let passive = false;
    /** @type {EventListenerSignal | undefined} */
    let signal;
    if (typeof options === "object" && options !== null || typeof options === "function") {
      capture = Boolean(options.capture);
      once = Boolean(options.once);
      passive = Boolean(options.passive);
      signal = options.signal;
    } else {
      capture = Boolean(options);
    }
    if (signal !== undefined) {
      if (signal === null || typeof signal !== "object" || typeof signal.aborted !== "boolean"
          || typeof signal.addEventListener !== "function" || typeof signal.removeEventListener !== "function") {
        throw new TypeError("signal must be an AbortSignal");
      }
      if (signal.aborted) {
        return;
      }
    }
    if (callback === null || callback === undefined) {
      return;
    }
    for (const listener of this.#listeners) {
      if (listener.type === eventType && listener.callback === callback && listener.capture === capture) {
        return;
      }
    }
    /** @type {ListenerRegistration} */
    const listener = {type: eventType, callback, capture, once, passive, removed: false, abortCleanup: null};
    this.#listeners.push(listener);
    if (signal !== undefined) {
      const abortSignal = signal;
      const onAbort = () => {
        this.#removeListener(listener);
      };
      listener.abortCleanup = () => {
        abortSignal.removeEventListener("abort", onAbort);
      };
      try {
        abortSignal.addEventListener("abort", onAbort, {once: true});
      } catch (error) {
        this.#removeListener(listener);
        throw error;
      }
      if (abortSignal.aborted) {
        this.#removeListener(listener);
      }
    }
  }

  /**
   * @param {string} type
   * @param {EventListener | null} callback
   * @param {boolean | EventListenerOptions | null} [options]
   * @returns {void}
   */
  removeEventListener(type, callback, options) {
    if (arguments.length < 2) {
      throw new TypeError("removeEventListener requires a type and callback");
    }
    if (typeof type === "symbol") {
      throw new TypeError("Event type cannot be a Symbol");
    }
    const eventType = String(type);
    if (callback !== null && callback !== undefined && typeof callback !== "function" && typeof callback !== "object") {
      throw new TypeError("Event listener must be a function or object");
    }
    let capture = false;
    if (typeof options === "object" && options !== null || typeof options === "function") {
      capture = Boolean(options.capture);
    } else {
      capture = Boolean(options);
    }
    for (const listener of this.#listeners) {
      if (listener.type === eventType && listener.callback === callback && listener.capture === capture) {
        this.#removeListener(listener);
        return;
      }
    }
  }

  /**
   * @param {ListenerRegistration} listener
   * @returns {void}
   */
  #removeListener(listener) {
    listener.removed = true;
    const index = this.#listeners.indexOf(listener);
    if (index !== -1) {
      this.#listeners.splice(index, 1);
    }
    const abortCleanup = listener.abortCleanup;
    listener.abortCleanup = null;
    if (abortCleanup !== null) {
      abortCleanup();
    }
  }

  /**
   * No parent chain: capture listeners run first, then noncapture listeners,
   * both at AT_TARGET. A snapshot per phase defers newly added listeners until
   * a later phase/dispatch; removed registrations are skipped immediately.
   * @param {Event} event
   * @returns {boolean}
   */
  dispatchEvent(event) {
    const eventState = eventStates.get(event);
    if (eventState === undefined) {
      throw new TypeError("dispatchEvent requires an Event");
    }
    if (eventState.dispatching) {
      // DOMException is not provided by this module; retain its standard name.
      const error = new Error("Event is already being dispatched");
      error.name = "InvalidStateError";
      throw error;
    }
    eventState.dispatching = true;
    eventState.target = this;
    eventState.currentTarget = this;
    eventState.eventPhase = Event.AT_TARGET;
    try {
      for (const capture of [true, false]) {
        if (eventState.propagationStopped) {
          break;
        }
        const listeners = this.#listeners.slice();
        for (const listener of listeners) {
          if (listener.removed || listener.type !== eventState.type || listener.capture !== capture) {
            continue;
          }
          if (listener.once) {
            this.#removeListener(listener);
          }
          eventState.inPassiveListener = listener.passive;
          try {
            if (typeof listener.callback === "function") {
              listener.callback.call(this, event);
            } else {
              // Resolve handleEvent at invocation time, not registration time.
              listener.callback.handleEvent(event);
            }
          } catch (error) {
            try {
              eventErrorReporter(error);
            } catch (reporterError) {
              void Promise.reject(reporterError);
            }
          } finally {
            eventState.inPassiveListener = false;
          }
          if (eventState.immediatePropagationStopped) {
            break;
          }
        }
      }
    } finally {
      eventState.dispatching = false;
      eventState.currentTarget = null;
      eventState.eventPhase = Event.NONE;
      eventState.propagationStopped = false;
      eventState.immediatePropagationStopped = false;
      eventState.inPassiveListener = false;
    }
    return !eventState.canceled;
  }

  get [Symbol.toStringTag]() {
    return "EventTarget";
  }
}
