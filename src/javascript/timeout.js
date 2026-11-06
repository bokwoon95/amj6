// @ts-check
// Public timer semantics over the C++ scheduler-backed implementation.
import { timeoutClear, timeoutSet } from "amj:timeout_impl";

/**
 * @param {(...args: Array<unknown>) => void} callback
 * @param {number} [delay]
 * @param {Array<unknown>} args
 * @returns {number}
 */
export function setTimeout(callback, delay = 0, ...args) {
  if (typeof callback !== "function") {
    throw new TypeError("setTimeout requires a function callback");
  }
  if (args.length > 256) {
    throw new RangeError("Timer argument limit exceeded");
  }
  let delayMs = +delay;
  if (Number.isNaN(delayMs) || delayMs < 0) {
    delayMs = 0;
  } else if (delayMs > 2147483647) {
    delayMs = 2147483647;
  }
  delayMs = Math.trunc(delayMs);
  const timerID = timeoutSet(function () {
    callback(...args);
  }, delayMs);
  return timerID;
}

/**
 * @param {number} [id]
 * @returns {void}
 */
export function clearTimeout(id) {
  if (id === undefined) {
    return;
  }
  const timerID = +id;
  if (!Number.isSafeInteger(timerID) || timerID < 1) {
    return;
  }
  timeoutClear(timerID);
}
