// @ts-check
// Cooperative cancellation over the JS event and timer modules. No native
// abort binding or global installation. Default reasons use named Error objects
// until the runtime provides DOMException.
import { Event, EventTarget } from "amj:event";
import { setTimeout } from "amj:timeout";

/**
 * @typedef {object} AbortState
 * @property {boolean} aborted
 * @property {unknown} reason
 * @property {Set<AbortSignal>} dependents
 * @property {Set<AbortSignal>} sources
 */

/** @type {WeakMap<AbortSignal, AbortState>} */
const abortStates = new WeakMap();
const signalToken = Symbol("AbortSignal construction");

/**
 * Mark the entire dependency graph before dispatching events. Composition must
 * not depend on ordinary listeners, which can be stopped or removed by scripts.
 * @param {AbortSignal} signal
 * @param {unknown} reason
 * @returns {void}
 */
function abortSignal(signal, reason) {
  const initialState = abortStates.get(signal);
  if (initialState === undefined) {
    throw new TypeError("Expected an AbortSignal");
  }
  if (initialState.aborted) {
    return;
  }
  if (reason === undefined) {
    const error = new Error("The operation was aborted");
    error.name = "AbortError";
    reason = error;
  }
  initialState.aborted = true;
  initialState.reason = reason;
  const pending = [signal];
  // Array iteration includes dependent signals appended during traversal.
  for (const currentSignal of pending) {
    const state = abortStates.get(currentSignal);
    if (state === undefined) {
      throw new TypeError("Missing AbortSignal state");
    }
    for (const dependent of state.dependents) {
      const dependentState = abortStates.get(dependent);
      if (dependentState === undefined || dependentState.aborted) {
        continue;
      }
      dependentState.aborted = true;
      dependentState.reason = reason;
      pending.push(dependent);
    }
    state.dependents.clear();
  }
  for (const currentSignal of pending) {
    const state = abortStates.get(currentSignal);
    if (state === undefined) {
      throw new TypeError("Missing AbortSignal state");
    }
    for (const source of state.sources) {
      abortStates.get(source)?.dependents.delete(currentSignal);
    }
    state.sources.clear();
    EventTarget.prototype.dispatchEvent.call(currentSignal, new Event("abort"));
  }
}

export class AbortSignal extends EventTarget {
  /** @type {AbortState} */
  #state = {
    aborted: false,
    reason: undefined,
    dependents: new Set(),
    sources: new Set(),
  };

  /** @type {((this: AbortSignal, event: Event) => unknown) | null} */
  #onabort = null;

  /** @type {((event: Event) => void) | null} */
  #onabortListener = null;

  /**
   * Not publicly constructible. Use AbortController or the static factories.
   * @param {symbol} token
   */
  constructor(token) {
    super();
    if (token !== signalToken) {
      throw new TypeError("Illegal AbortSignal constructor");
    }
    abortStates.set(this, this.#state);
  }

  get aborted() {
    return this.#state.aborted;
  }

  get reason() {
    return this.#state.reason;
  }

  get onabort() {
    return this.#onabort;
  }

  /** @param {((this: AbortSignal, event: Event) => unknown) | null} callback */
  set onabort(callback) {
    if (typeof callback !== "function") {
      callback = null;
    }
    this.#onabort = callback;
    if (callback === null) {
      if (this.#onabortListener !== null) {
        EventTarget.prototype.removeEventListener.call(this, "abort", this.#onabortListener);
        this.#onabortListener = null;
      }
    } else if (this.#onabortListener === null) {
      // Replacing a non-null handler preserves its position in listener order.
      this.#onabortListener = (event) => {
        this.#onabort?.call(this, event);
      };
      EventTarget.prototype.addEventListener.call(this, "abort", this.#onabortListener);
    }
  }

  throwIfAborted() {
    if (this.#state.aborted) {
      throw this.#state.reason;
    }
  }

  /**
   * @param {unknown} [reason]
   * @returns {AbortSignal}
   */
  static abort(reason) {
    const signal = new AbortSignal(signalToken);
    abortSignal(signal, reason);
    return signal;
  }

  /**
   * Uses native timer chunks so delays above the timer API's int32 limit do not
   * abort prematurely. Zero still aborts asynchronously on a later timer turn.
   * @param {number} milliseconds
   * @returns {AbortSignal}
   */
  static timeout(milliseconds) {
    if (arguments.length === 0) {
      throw new TypeError("AbortSignal.timeout requires a delay");
    }
    const converted = +milliseconds;
    const delay = Math.trunc(converted);
    if (!Number.isFinite(converted) || delay < 0 || delay > Number.MAX_SAFE_INTEGER) {
      throw new TypeError("AbortSignal timeout must be in [0, 9007199254740991] milliseconds");
    }
    const signal = new AbortSignal(signalToken);
    /** @type {{now: () => number}} */
    const performance = Reflect.get(globalThis, "performance");
    const deadline = performance.now() + delay;
    const onTimeout = () => {
      const remaining = Math.ceil(deadline - performance.now());
      if (remaining > 0) {
        setTimeout(onTimeout, Math.min(remaining, 2147483647));
        return;
      }
      const error = new Error("The operation timed out");
      error.name = "TimeoutError";
      abortSignal(signal, error);
    };
    setTimeout(onTimeout, Math.min(delay, 2147483647));
    return signal;
  }

  /**
   * First already-aborted input wins; otherwise the first future abort wins.
   * Dependency links are internal, not stoppable abort event listeners. Links
   * are detached when the dependent aborts; pending sources retain dependents.
   * @param {Iterable<AbortSignal>} signals
   * @returns {AbortSignal}
   */
  static any(signals) {
    if (signals === null || signals === undefined || typeof signals[Symbol.iterator] !== "function") {
      throw new TypeError("AbortSignal.any requires an iterable of signals");
    }
    const sources = Array.from(signals);
    // Validate every member before inspecting aborted state or subscribing.
    for (const source of sources) {
      if (!abortStates.has(source)) {
        throw new TypeError("AbortSignal.any requires AbortSignal instances");
      }
    }
    const signal = new AbortSignal(signalToken);
    for (const source of sources) {
      const state = abortStates.get(source);
      if (state === undefined) {
        throw new TypeError("Missing AbortSignal state");
      }
      if (state.aborted) {
        abortSignal(signal, state.reason);
        return signal;
      }
    }
    for (const source of sources) {
      if (signal.#state.sources.has(source)) {
        continue;
      }
      signal.#state.sources.add(source);
      const state = abortStates.get(source);
      if (state === undefined) {
        throw new TypeError("Missing AbortSignal state");
      }
      state.dependents.add(signal);
    }
    return signal;
  }

  get [Symbol.toStringTag]() {
    return "AbortSignal";
  }
}

export class AbortController {
  #signal = new AbortSignal(signalToken);

  get signal() {
    return this.#signal;
  }

  /**
   * Idempotent; the first abort preserves its reason and synchronously notifies.
   * @param {unknown} [reason]
   * @returns {void}
   */
  abort(reason) {
    abortSignal(this.#signal, reason);
  }

  get [Symbol.toStringTag]() {
    return "AbortController";
  }
}
