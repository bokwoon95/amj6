// @ts-check
// Fetch over FetchImpl::{start,read,close}. JS owns Promises and API semantics.
// Dependencies are module imports, never target/global lookups.

import { fetchImpl } from "amj:fetch_impl";
import { URL } from "amj:url";
import { TextEncoder, TextDecoder } from "amj:text";

/**
 * @typedef {import("amj:fetch_impl").FetchRequestHandle} FetchRequestHandle
 */
/**
 * @typedef {import("amj:fetch_impl").FetchFailure} FetchFailure
 */
/**
 * @typedef {import("amj:fetch_impl").FetchResponseMetadata} FetchResponseMetadata
 */
/**
 * @template T
 * @typedef {object} Pending
 * @property {(value: T) => void} resolve
 * @property {(reason: unknown) => void} reject
 */
/**
 * @typedef {{done: true, value: undefined} | {done: false, value: Uint8Array}} ReadResult
 */
/**
 * @typedef {{once?: boolean}} ListenerOptions
 */
/**
 * Minimal AbortSignal contract; no browser DOM library is assumed.
 * @typedef {object} AbortSignalValue
 * @property {boolean} aborted
 * @property {unknown} [reason]
 * @property {(this: AbortSignalValue, type: "abort", listener: () => void, options?: ListenerOptions) => void} addEventListener
 * @property {(this: AbortSignalValue, type: "abort", listener: () => void) => void} removeEventListener
 */

/**
 * @typedef {object} FetchInit
 * @property {string} [method]
 * @property {Iterable<readonly [unknown, unknown]> | Record<string, unknown> | null} [headers]
 * @property {string | Uint8Array | null} [body]
 * @property {AbortSignalValue | null} [signal]
 * @property {"error"} [redirect]
 * @property {undefined} [credentials]
 * @property {undefined} [cache]
 * @property {undefined} [integrity]
 * @property {undefined} [keepalive]
 * @property {undefined} [mode]
 */
/**
 * @typedef {Error & {errorType: string, errorMessage: string}} FetchError
 */
const bodyLimit = 33554432;
/**
 * @type {WeakSet<Headers>}
 */
const immutable = new WeakSet();
/**
 * @param {unknown} value
 * @returns {string}
 */
const normalizeName = (value) => {
  if (typeof value === "symbol") {
    throw new TypeError("Invalid header name");
  }
  const name = String(value).toLowerCase();
  if (!/^[!#$%&'*+.^_`|~0-9a-z-]+$/.test(name)) {
    throw new TypeError("Invalid header name");
  }
  return name;
};
/**
 * @param {unknown} value
 * @returns {string}
 */
const normalizeValue = (value) => {
  if (typeof value === "symbol") {
    throw new TypeError("Invalid header value");
  }
  const text = String(value);
  if (/[\x00-\x08\x0a-\x1f\x7f]/.test(text)) {
    throw new TypeError("Invalid header value");
  }
  return text.replace(/^[\t ]+|[\t ]+$/g, "");
};
/**
 * @param {FetchFailure} failure
 * @returns {FetchError}
 */
const completionError = (failure) => {
  let error;
  if (failure.type === "LimitExceeded") {
    error = new RangeError(failure.message);
  } else {
    error = new TypeError(failure.message);
  }
  return Object.assign(error, {
    errorType: failure.type,
    errorMessage: failure.message,
  });
};
export class Headers {
  /**
   * @type {Map<string, string>}
   */
  #values = new Map();
  /**
   * @param {unknown} [init]
   */
  constructor(init) {
    if (init === undefined || init === null) {
      return;
    }
    if (typeof init !== "object" && typeof init !== "function") {
      throw new TypeError("Invalid Headers initializer");
    }
    const properties =
      /**
       * @type {Record<PropertyKey, unknown>}
       */ (init);
    if (typeof properties[Symbol.iterator] === "function") {
      // Iteration checks the dynamic protocol; fields remain unknown.
      for (const pair of /**
       * @type {Iterable<unknown>}
       */ (init)) {
        if (pair === null || (typeof pair !== "object" && typeof pair !== "function")) {
          throw new TypeError("Headers require name/value pairs");
        }
        const fields = Array.from(
          /**
           * @type {Iterable<unknown>}
           */ (pair),
        );
        if (fields.length !== 2) {
          throw new TypeError("Headers require name/value pairs");
        }
        this.append(fields[0], fields[1]);
      }
    } else {
      for (const [name, value] of Object.entries(properties)) {
        this.append(name, value);
      }
    }
  }
  /**
   * @param {unknown} name
   * @param {unknown} value
   * @returns {void}
   */
  append(name, value) {
    if (arguments.length < 2) {
      throw new TypeError("append requires a name and value");
    }
    if (immutable.has(this)) {
      throw new TypeError("Response headers are immutable");
    }
    const nameText = normalizeName(name);
    let valueText = normalizeValue(value);
    const previous = this.#values.get(nameText);
    if (previous !== undefined) {
      valueText = previous + ", " + valueText;
    }
    this.#values.set(nameText, valueText);
  }
  /**
   * @param {unknown} name
   * @param {unknown} value
   * @returns {void}
   */
  set(name, value) {
    if (arguments.length < 2) {
      throw new TypeError("set requires a name and value");
    }
    if (immutable.has(this)) {
      throw new TypeError("Response headers are immutable");
    }
    this.#values.set(normalizeName(name), normalizeValue(value));
  }
  /**
   * @param {unknown} name
   * @returns {void}
   */
  delete(name) {
    if (arguments.length < 1) {
      throw new TypeError("delete requires a name");
    }
    if (immutable.has(this)) {
      throw new TypeError("Response headers are immutable");
    }
    this.#values.delete(normalizeName(name));
  }
  /**
   * @param {unknown} name
   * @returns {string | null}
   */
  get(name) {
    if (arguments.length < 1) {
      throw new TypeError("get requires a name");
    }
    return this.#values.get(normalizeName(name)) ?? null;
  }
  /**
   * @param {unknown} name
   * @returns {boolean}
   */
  has(name) {
    if (arguments.length < 1) {
      throw new TypeError("has requires a name");
    }
    return this.#values.has(normalizeName(name));
  }
  /**
   * @returns {Generator<[string, string], void, undefined>}
   */
  *entries() {
    yield* Array.from(this.#values).sort((left, right) => {
      if (left[0] < right[0]) {
        return -1;
      }
      if (left[0] > right[0]) {
        return 1;
      }
      return 0;
    });
  }
  /**
   * @returns {Generator<string, void, unknown>}
   */
  *keys() {
    for (const [name] of this.entries()) {
      yield name;
    }
  }
  /**
   * @returns {Generator<string, void, unknown>}
   */
  *values() {
    for (const [, value] of this.entries()) {
      yield value;
    }
  }
  [Symbol.iterator]() {
    return this.entries();
  }
  /**
   * @param {(this: unknown, value: string, name: string, headers: Headers) => void} callback
   * @param {unknown} [thisArg]
   * @returns {void}
   */
  forEach(callback, thisArg) {
    if (typeof callback !== "function") {
      throw new TypeError("Expected a callback");
    }
    for (const [name, value] of this.entries()) {
      callback.call(thisArg, value, name, this);
    }
  }
}
class RequestState {
  /**
   * @type {FetchRequestHandle | null}
   */
  request = null;
  /**
   * @type {Pending<FetchResponseMetadata> | null}
   */
  headers = null;
  /**
   * @type {Pending<ReadResult> | null}
   */
  reading = null;
  ended = false;
  failed = false;
  /**
   * @type {unknown}
   */
  reason = undefined;
  disturbed = false;
  /**
   * @type {Reader | null}
   */
  reader = null;
  /**
   * @type {AbortSignalValue | null}
   */
  signal = null;
  /**
   * @type {(() => void) | null}
   */
  listener = null;
  /**
   * @type {AbortSignalValue["removeEventListener"] | null}
   */
  removeListener = null;
  /**
   * @param {AbortSignalValue | null} signal
   * @returns {void}
   */
  attachSignal(signal) {
    if (signal === null) {
      return;
    }
    const addListener = signal.addEventListener;
    const removeListener = signal.removeEventListener;
    if (
      typeof addListener !== "function" ||
      typeof removeListener !== "function" ||
      typeof signal.aborted !== "boolean"
    ) {
      throw new TypeError("Expected an AbortSignal");
    }
    this.signal = signal;
    this.removeListener = removeListener;
    const listener = () => {
      if (this.ended) {
        return;
      }
      let reason = signal.reason;
      if (reason === undefined) {
        const error = new Error("The operation was aborted");
        error.name = "AbortError";
        reason = error;
      }
      this.fail(reason);
    };
    this.listener = listener;
    addListener.call(signal, "abort", listener, { once: true });
    if (signal.aborted) {
      listener();
    }
  }
  dispose() {
    const request = this.request;
    this.request = null;
    try {
      if (request !== null) {
        fetchImpl.close(request);
      }
    } finally {
      const signal = this.signal;
      const listener = this.listener;
      const removeListener = this.removeListener;
      this.signal = null;
      this.listener = null;
      this.removeListener = null;
      if (signal !== null) {
        if (listener === null || removeListener === null) {
          throw new TypeError("Missing abort subscription");
        }
        removeListener.call(signal, "abort", listener);
      }
    }
  }
  /**
   * @param {unknown} reason
   * @returns {void}
   */
  fail(reason) {
    if (this.ended) {
      return;
    }
    this.ended = true;
    this.failed = true;
    this.reason = reason;
    const headers = this.headers;
    const reading = this.reading;
    this.headers = null;
    this.reading = null;
    if (headers !== null) {
      headers.reject(reason);
    }
    if (reading !== null) {
      reading.reject(reason);
    }
    this.dispose();
  }
  cancel() {
    if (this.failed) {
      throw this.reason;
    }
    if (this.ended) {
      return;
    }
    this.ended = true;
    const reading = this.reading;
    this.reading = null;
    if (reading !== null) {
      reading.resolve({ value: undefined, done: true });
    }
    this.dispose();
  }
  finish() {
    this.ended = true;
    this.dispose();
  }
  /**
   * @param {FetchFailure | null} failure
   * @param {FetchResponseMetadata | null} metadata
   * @returns {void}
   */
  receiveHeaders(failure, metadata) {
    const headers = this.headers;
    this.headers = null;
    if (headers === null) {
      return;
    }
    if (failure !== null || metadata === null) {
      let error;
      if (failure !== null) {
        error = completionError(failure);
      } else {
        error = new TypeError("Missing native response metadata");
      }
      headers.reject(error);
      this.fail(error);
    } else {
      headers.resolve(metadata);
    }
  }
  /**
   * @returns {Promise<ReadResult>}
   */
  async read() {
    if (this.failed) {
      throw this.reason;
    }
    if (this.ended) {
      return { value: undefined, done: true };
    }
    if (this.reading !== null) {
      throw new TypeError("Only one outstanding read is supported");
    }
    const request = this.request;
    if (request === null) {
      throw new TypeError("HTTP request is closed");
    }
    return new Promise((resolve, reject) => {
      this.reading = { resolve, reject };
      try {
        fetchImpl.read(request, (failure, chunk) => {
          const reading = this.reading;
          this.reading = null;
          if (reading === null) {
            return;
          }
          if (failure !== null) {
            const error = completionError(failure);
            reading.reject(error);
            this.fail(error);
          } else if (chunk === null) {
            reading.resolve({ value: undefined, done: true });
            this.finish();
          } else if (chunk instanceof Uint8Array && chunk.length !== 0) {
            reading.resolve({ value: chunk, done: false });
          } else {
            const error = new TypeError("Invalid native response chunk");
            reading.reject(error);
            this.fail(error);
          }
        });
      } catch (error) {
        this.fail(error);
      }
    });
  }
}
class Reader {
  /**
   * @type {RequestState}
   */
  #state;
  #pending = false;
  #released = false;
  /**
   * @param {RequestState} state
   */
  constructor(state) {
    this.#state = state;
  }
  /**
   * @returns {Promise<ReadResult>}
   */
  async read() {
    if (this.#released) {
      throw new TypeError("Reader lock was released");
    }
    if (this.#pending) {
      throw new TypeError("Only one outstanding read is supported");
    }
    this.#state.disturbed = true;
    this.#pending = true;
    try {
      return await this.#state.read();
    } finally {
      this.#pending = false;
    }
  }
  /**
   * @param {unknown} [reason]
   * @returns {Promise<void>}
   */
  async cancel(reason) {
    if (this.#released) {
      throw new TypeError("Reader lock was released");
    }
    this.#state.disturbed = true;
    this.#state.cancel();
  }
  releaseLock() {
    if (this.#released) {
      return;
    }
    if (this.#pending) {
      throw new TypeError("Cannot release a reader with an outstanding read");
    }
    this.#released = true;
    this.#state.reader = null;
  }
}
class Body {
  /**
   * @type {RequestState}
   */
  #state;
  /**
   * @param {RequestState} state
   */
  constructor(state) {
    this.#state = state;
  }
  get locked() {
    return this.#state.reader !== null;
  }
  /**
   * @param {{mode?: unknown} | null} [options]
   * @returns {Reader}
   */
  getReader(options) {
    if (options && options.mode !== undefined) {
      throw new TypeError("BYOB readers are not supported");
    }
    if (this.locked) {
      throw new TypeError("Response body is locked");
    }
    const reader = new Reader(this.#state);
    this.#state.reader = reader;
    return reader;
  }
  /**
   * @param {unknown} [reason]
   * @returns {Promise<void>}
   */
  async cancel(reason) {
    if (this.locked) {
      throw new TypeError("Response body is locked");
    }
    this.#state.disturbed = true;
    this.#state.cancel();
  }
}
class Response {
  /**
   * @type {RequestState}
   */
  #state;
  /**
   * @type {number}
   */
  #status;
  /**
   * @type {Headers}
   */
  #headers;
  /**
   * @type {Body | null}
   */
  #body;
  /**
   * @type {string}
   */
  #url;
  /**
   * @param {RequestState} state
   * @param {FetchResponseMetadata} metadata
   * @param {string} url
   * @param {string} method
   */
  constructor(state, metadata, url, method) {
    this.#state = state;
    this.#status = metadata.status;
    this.#headers = new Headers(metadata.headers);
    immutable.add(this.#headers);
    this.#url = url;
    this.#body = new Body(state);
    if (method === "HEAD" || [204, 205, 304].includes(metadata.status)) {
      this.#body = null;
      state.finish();
    }
  }
  get status() {
    return this.#status;
  }
  get ok() {
    return this.#status >= 200 && this.#status <= 299;
  }
  get headers() {
    return this.#headers;
  }
  get url() {
    return this.#url;
  }
  get body() {
    return this.#body;
  }
  get bodyUsed() {
    return this.#state.disturbed;
  }
  /**
   * @returns {Promise<string>}
   */
  async text() {
    if (this.#body === null) {
      return "";
    }
    if (this.bodyUsed || this.#body.locked) {
      throw new TypeError("Response body is already used or locked");
    }
    const decoder = new TextDecoder();
    const reader = this.#body.getReader();
    /**
     * @type {Array<string>}
     */
    const parts = [];
    let bytes = 0;
    try {
      while (true) {
        const chunk = await reader.read();
        if (chunk.done) {
          break;
        }
        bytes += chunk.value.byteLength;
        if (bytes > bodyLimit) {
          throw new RangeError("Response body exceeds 32 MiB");
        }
        parts.push(decoder.decode(chunk.value, { stream: true }));
      }
      parts.push(decoder.decode());
      return parts.join("");
    } catch (error) {
      if (!this.#state.ended) {
        await reader.cancel(error);
      }
      throw error;
    } finally {
      reader.releaseLock();
    }
  }
  /**
   * @returns {Promise<unknown>}
   */
  async json() {
    return JSON.parse(await this.text());
  }
}
/**
 * @param {unknown} input
 * @param {FetchInit | null} [init]
 * @returns {Promise<Response>}
 */
export async function fetch(input, init) {
  if (init === undefined || init === null) {
    init = {};
  }
  if (typeof input === "symbol") {
    throw new TypeError("Invalid fetch URL");
  }
  const url = new URL(String(input));
  if (url.protocol !== "http:" && url.protocol !== "https:") {
    throw new TypeError("Only HTTP(S) URLs are supported");
  }
  if (url.username || url.password) {
    throw new TypeError("URL credentials are not supported");
  }
  url.hash = "";
  let method = "GET";
  if (init.method !== undefined) {
    if (typeof init.method === "symbol") {
      throw new TypeError("Invalid HTTP method");
    }
    method = String(init.method).toUpperCase();
  }
  if (!/^[!#$%&'*+.^_`|~0-9A-Z-]+$/.test(method) || ["CONNECT", "TRACE", "TRACK"].includes(method)) {
    throw new TypeError("Invalid HTTP method");
  }
  if (init.redirect !== undefined && init.redirect !== "error") {
    throw new TypeError("Only redirect:error is supported");
  }
  for (const name of /**
   * @type {const}
   */ (["credentials", "cache", "integrity", "keepalive", "mode"])) {
    if (init[name] !== undefined) {
      throw new TypeError(`Fetch option ${name} is not supported`);
    }
  }
  const headers = new Headers(init.headers);
  /**
   * @type {Uint8Array}
   */
  let body = new Uint8Array(0);
  if (init.body !== undefined && init.body !== null) {
    if (method === "GET" || method === "HEAD") {
      throw new TypeError("GET/HEAD cannot have a body");
    }
    if (typeof init.body === "string") {
      body = new TextEncoder().encode(init.body);
      if (!headers.has("content-type")) {
        headers.set("content-type", "text/plain;charset=UTF-8");
      }
    } else if (init.body instanceof Uint8Array) {
      body = init.body.slice();
    } else {
      throw new TypeError("Only string/Uint8Array bodies are supported");
    }
  }
  if (body.byteLength > bodyLimit) {
    throw new RangeError("Request body exceeds 32 MiB");
  }
  /**
   * @type {AbortSignalValue | null}
   */
  let signal = null;
  if (init.signal !== undefined && init.signal !== null) {
    signal = init.signal;
  }
  if (signal !== null && signal.aborted) {
    let reason = signal.reason;
    if (reason === undefined) {
      const error = new Error("The operation was aborted");
      error.name = "AbortError";
      reason = error;
    }
    throw reason;
  }
  const state = new RequestState();
  try {
    /**
     * @type {FetchResponseMetadata}
     */
    const metadata = await new Promise((resolve, reject) => {
      state.headers = { resolve, reject };
      try {
        state.request = fetchImpl.start(url.href, method, Array.from(headers), body, (failure, metadata) =>
          state.receiveHeaders(failure, metadata),
        );
        state.attachSignal(signal);
      } catch (error) {
        state.fail(error);
      }
    });
    if (state.failed) {
      throw state.reason;
    }
    return new Response(state, metadata, url.href, method);
  } catch (error) {
    state.fail(error);
    throw error;
  }
}
