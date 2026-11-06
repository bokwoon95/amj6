// @ts-check
// Public URL API over the native URL and query-string functions.
// Only Ada parsing/encoding crosses into C++; JS owns all object behavior.

import { queryStringParse, queryStringify, urlParse, urlWithComponent } from "amj:url_impl";

/**
 * @typedef {import("amj:url_impl").URLParts} URLParts
 */

/**
 * @type {(urlSearchParams: URLSearchParams) => string}
 */
let getQueryString;

/**
 * @type {(urlSearchParams: URLSearchParams, queryString: string) => void}
 */
let setQueryString;

export class URLSearchParams {
  /**
   * @type {Array<[string, string]>}
   */
  #entries = [];

  #queryString = "";

  static {
    getQueryString = function (urlSearchParams) {
      return urlSearchParams.#queryString;
    };
    setQueryString = function (urlSearchParams, queryString) {
      const entries = queryStringParse(queryString);
      urlSearchParams.#entries = entries;
      urlSearchParams.#queryString = queryString;
    };
  }

  /**
   * @param {unknown} [options]
   */
  constructor(options) {
    if (options === null || options === undefined) {
      return;
    }
    if (typeof options !== "object" && typeof options !== "function") {
      this.#entries = queryStringParse(stringify(options));
      return;
    }
    const entriesIteratorFunction = Reflect.get(options, Symbol.iterator);
    if (entriesIteratorFunction === null || entriesIteratorFunction === undefined) {
      for (const name of Object.keys(options)) {
        if (this.#entries.length > 65535) {
          throw new RangeError("URLSearchParams.constructor: queryString has too many entries");
        }
        this.#entries.push([stringify(name), stringify(Reflect.get(options, name))]);
      }
      return;
    }
    if (typeof entriesIteratorFunction !== "function") {
      throw new TypeError("URLSearchParams.constructor: options is not an iterable");
    }
    const entriesIterable = {
      [Symbol.iterator]: function () {
        return Reflect.apply(entriesIteratorFunction, options, arguments);
      },
    };
    let i = -1;
    for (const entry of entriesIterable) {
      i++;
      if (this.#entries.length > 65535) {
        throw new RangeError("URLSearchParams.constructor: queryString has too many entries");
      }
      if (entry === null) {
        throw new TypeError(`URLSearchParams.constructor: entry #${i + 1} is null`);
      }
      if (typeof entry !== "object" && typeof entry !== "function") {
        throw new TypeError(`URLSearchParams.constructor: entry #${i + 1} is not an iterable`);
      }
      const entryIteratorFunction = Reflect.get(entry, Symbol.iterator);
      if (typeof entryIteratorFunction != "function") {
        throw new TypeError(`URLSearchParams.constructor: entry #${i + 1} is not an iterable`);
      }
      const entryIterable = {
        [Symbol.iterator]: function () {
          return Reflect.apply(entryIteratorFunction, entry, arguments);
        },
      };
      let j = -1;
      let name = "";
      let value = "";
      for (const field of entryIterable) {
        j++;
        if (j === 0) {
          name = stringify(field);
        } else if (j === 1) {
          value = stringify(field);
        } else {
          throw new TypeError(`URLSearchParams.constructor: entry #${i + 1} has more than two fields`);
        }
      }
      if (j < 1) {
        throw new TypeError(`URLSearchParams.constructor: entry #${i + 1} has less than two fields`);
      }
      this.#entries.push([name, value]);
    }
  }

  get [Symbol.toStringTag]() {
    return "URLSearchParams";
  }

  get size() {
    return this.#entries.length;
  }

  [Symbol.iterator]() {
    return this.entries();
  }

  /**
   * @param {unknown} name
   * @param {unknown} value
   * @returns {void}
   */
  append(name, value) {
    if (arguments.length < 2) {
      throw new TypeError(`URLSearchParams.append: requires at least 2 arguments (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    const valueStr = stringify(value);
    const entries = Array.from(this.#entries);
    entries.push([nameStr, valueStr]);
    const queryString = queryStringify(entries);
    this.#entries = entries;
    this.#queryString = "?" + queryString;
  }

  /**
   * @param {unknown} name
   * @param {unknown} [value]
   * @returns {void}
   */
  delete(name, value) {
    if (arguments.length < 1) {
      throw new TypeError(`URLSearchParams.delete: requires at least 1 argument (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    const valueStr = stringify(value);
    const entries = [];
    for (const entry of this.#entries) {
      if (entry[0] === nameStr) {
        if (value === undefined) {
          continue;
        }
        if (entry[1] === valueStr) {
          continue;
        }
      }
      entries.push(entry);
    }
    const queryString = queryStringify(entries);
    this.#entries = entries;
    this.#queryString = "?" + queryString;
    if (this.#queryString === "?") {
      this.#queryString = "";
    }
  }

  /**
   * @returns {Generator<[string, string], void, unknown>}
   */
  *entries() {
    // Use a for-let loop instead of for-of loop to avoid looping over a
    // captured copy of the array; accessing array elements by index give us a
    // live view.
    for (let i = 0; i < this.#entries.length; i++) {
      const entry = this.#entries[i];
      if (entry === undefined) {
        throw new RangeError(`URLSearchParams.entries: index ${i} out of bounds (length = ${this.#entries.length})`);
      }
      yield [entry[0], entry[1]];
    }
  }

  /**
   * @param {(value: string, key: string, searchParams: URLSearchParams) => void} callback
   * @param {unknown} [thisArg]
   * @returns {void}
   */
  forEach(callback, thisArg) {
    if (arguments.length < 1) {
      throw new TypeError(`URLSearchParams.forEach: requires at least 1 argument (${arguments.length} provided)`);
    }
    if (typeof callback !== "function") {
      throw new TypeError("URLSearchParams.forEach: callback is not a function");
    }
    // Use a for-let loop instead of for-of loop to avoid looping over a
    // captured copy of the array; accessing array elements by index give us a
    // live view.
    for (let i = 0; i < this.#entries.length; i++) {
      const entry = this.#entries[i];
      if (entry === undefined) {
        throw new RangeError(`URLSearchParams.entries: index ${i} out of bounds (length = ${this.#entries.length})`);
      }
      callback.call(thisArg, entry[1], entry[0], this);
    }
  }

  /**
   * @param {unknown} name
   * @returns {string | null}
   */
  get(name) {
    if (arguments.length < 1) {
      throw new TypeError(`URLSearchParams.get: requires at least 1 argument (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    for (const entry of this.#entries) {
      if (entry[0] === nameStr) {
        return entry[1];
      }
    }
    return null;
  }

  /**
   * @param {unknown} name
   * @returns {Array<string>}
   */
  getAll(name) {
    if (arguments.length < 1) {
      throw new TypeError(`URLSearchParams.getAll: requires at least 1 argument (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    const values = [];
    for (const entry of this.#entries) {
      if (entry[0] === nameStr) {
        values.push(entry[1]);
      }
    }
    return values;
  }

  /**
   * @param {unknown} name
   * @param {unknown} [value]
   * @returns {boolean}
   */
  has(name, value) {
    if (arguments.length < 1) {
      throw new TypeError(`URLSearchParams.has: requires at least 1 argument (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    const valueStr = stringify(value);
    for (const entry of this.#entries) {
      if (entry[0] !== nameStr) {
        continue;
      }
      if (value !== undefined) {
        if (entry[1] !== valueStr) {
          continue;
        }
      }
      return true;
    }
    return false;
  }

  /**
   * @returns {Generator<string, void, unknown>}
   */
  *keys() {
    // Use a for-let loop instead of for-of loop to avoid looping over a
    // captured copy of the array; accessing array elements by index give us a
    // live view.
    for (let i = 0; i < this.#entries.length; i++) {
      const entry = this.#entries[i];
      if (entry === undefined) {
        throw new RangeError(`URLSearchParams.keys: index ${i} out of bounds (length = ${this.#entries.length})`);
      }
      yield entry[0];
    }
  }

  /**
   * @param {unknown} name
   * @param {unknown} value
   * @returns {void}
   */
  set(name, value) {
    if (arguments.length < 2) {
      throw new TypeError(`URLSearchParams.set: requires at least 2 arguments (${arguments.length} provided)`);
    }
    const nameStr = stringify(name);
    const valueStr = stringify(value);
    /**
     * @type {Array<[string, string]>}
     */
    const entries = [];
    let found = false;
    for (const entry of this.#entries) {
      if (entry[0] !== nameStr) {
        entries.push(entry);
        continue;
      }
      if (found) {
        continue;
      }
      entries.push([nameStr, valueStr]);
      found = true;
    }
    if (!found) {
      entries.push([nameStr, valueStr]);
    }
    const queryString = queryStringify(entries);
    this.#entries = entries;
    this.#queryString = "?" + queryString;
  }

  sort() {
    const entries = Array.from(this.#entries);
    entries.sort((left, right) => {
      if (left[0] < right[0]) {
        return -1;
      }
      if (left[0] > right[0]) {
        return 1;
      }
      return 0;
    });
    const queryString = queryStringify(entries);
    this.#entries = entries;
    this.#queryString = "?" + queryString;
    if (this.#queryString === "?") {
      this.#queryString = "";
    }
  }

  toString() {
    return queryStringify(this.#entries);
  }

  /**
   * @returns {Generator<string, void, unknown>}
   */
  *values() {
    // Use a for-let loop instead of for-of loop to avoid looping over a
    // captured copy of the array; accessing array elements by index give us a
    // live view.
    for (let i = 0; i < this.#entries.length; i++) {
      const entry = this.#entries[i];
      if (entry === undefined) {
        throw new RangeError(`URLSearchParams.values: index ${i} out of bounds (length = ${this.#entries.length})`);
      }
      yield entry[1];
    }
  }
}

// Web IDL requires Symbol.iterator and entries to be the same function.
// Replace the class wrapper, which exists so TypeScript recognizes iterability.
// https://webidl.spec.whatwg.org/#es-iterable
Object.defineProperty(URLSearchParams.prototype, Symbol.iterator, {
  value: URLSearchParams.prototype.entries,
  writable: true,
  configurable: true,
});

export class URL {
  #href = "";

  /**
   * @type {URLSearchParams}
   */
  #urlSearchParams;

  /**
   * @param {unknown} input
   * @param {unknown} [base]
   */
  constructor(input, base) {
    if (arguments.length < 1) {
      throw new TypeError(`URL.constructor: requires at least 1 argument (${arguments.length} provided)`);
    }
    const inputStr = stringify(input);
    const baseStr = base === undefined ? undefined : stringify(base);
    const parts = urlParse(inputStr, baseStr);
    if (parts === null) {
      throw new TypeError("URL.constructor: invalid URL");
    }
    const hashIndex = parts.href.indexOf("#");
    let queryEnd = parts.href.length;
    if (hashIndex !== -1) {
      queryEnd = hashIndex;
    }
    const queryIndex = parts.href.indexOf("?");
    let queryString = "";
    let href = parts.href;
    if (queryIndex !== -1 && queryIndex < queryEnd) {
      queryString = parts.href.slice(queryIndex, queryEnd);
      href = parts.href.slice(0, queryIndex + 1) + parts.href.slice(queryEnd);
    }
    this.#urlSearchParams = new URLSearchParams();
    setQueryString(this.#urlSearchParams, queryString);
    this.#href = href;
  }

  get [Symbol.toStringTag]() {
    return "URL";
  }

  get hash() {
    return this.#getURLParts().hash;
  }

  set hash(value) {
    this.#setComponent("hash", value);
  }

  get host() {
    return this.#getURLParts().host;
  }

  set host(value) {
    this.#setComponent("host", value);
  }

  get hostname() {
    return this.#getURLParts().hostname;
  }

  set hostname(value) {
    this.#setComponent("hostname", value);
  }

  get href() {
    return this.#getURLParts().href;
  }

  set href(value) {
    this.#setComponent("href", value);
  }

  get origin() {
    return this.#getURLParts().origin;
  }

  get password() {
    return this.#getURLParts().password;
  }

  set password(value) {
    this.#setComponent("password", value);
  }

  get pathname() {
    return this.#getURLParts().pathname;
  }

  set pathname(value) {
    this.#setComponent("pathname", value);
  }

  get port() {
    return this.#getURLParts().port;
  }

  set port(value) {
    this.#setComponent("port", value);
  }

  get protocol() {
    return this.#getURLParts().protocol;
  }

  set protocol(value) {
    this.#setComponent("protocol", value);
  }

  get search() {
    return this.#getURLParts().search;
  }

  set search(value) {
    this.#setComponent("search", value);
  }

  get searchParams() {
    return this.#urlSearchParams;
  }

  get username() {
    return this.#getURLParts().username;
  }

  set username(value) {
    this.#setComponent("username", value);
  }

  /**
   * @param {unknown} input
   * @param {unknown} [base]
   * @returns {boolean}
   */
  static canParse(input, base) {
    if (arguments.length < 1) {
      throw new TypeError(`URL.canParse: requires at least 1 argument (${arguments.length} provided)`);
    }
    const inputStr = stringify(input);
    const baseStr = base === undefined ? undefined : stringify(base);
    return urlParse(inputStr, baseStr) !== null;
  }

  /**
   * @param {unknown} input
   * @param {unknown} [base]
   * @returns {URL | null}
   */
  static parse(input, base) {
    if (arguments.length < 1) {
      throw new TypeError(`URL.parse: requires at least 1 argument (${arguments.length} provided)`);
    }
    const inputStr = stringify(input);
    const baseStr = base === undefined ? undefined : stringify(base);
    const urlParts = urlParse(inputStr, baseStr);
    if (urlParts === null) {
      return null;
    }
    return new URL(urlParts.href);
  }

  toString() {
    return this.#getURLParts().href;
  }

  toJSON() {
    return this.#getURLParts().href;
  }

  /**
   * @returns {URLParts}
   */
  #getURLParts() {
    const urlParts = urlWithComponent(this.#href, "search", getQueryString(this.#urlSearchParams));
    if (urlParts === null) {
      throw new TypeError("URL.#getURLParts: invalid URL");
    }
    return urlParts;
  }

  /**
   * @param {string} component
   * @param {unknown} value
   * @returns {void}
   */
  #setComponent(component, value) {
    const valueStr = stringify(value);
    const urlParts = urlWithComponent(this.#getURLParts().href, component, valueStr);
    if (urlParts === null) {
      if (component === "href") {
        throw new TypeError("URL.href: invalid URL");
      }
      return;
    }
    // Extract from href to distinguish a bare '?' from no query.
    const hashIndex = urlParts.href.indexOf("#");
    let queryEnd = urlParts.href.length;
    if (hashIndex !== -1) {
      queryEnd = hashIndex;
    }
    const queryIndex = urlParts.href.indexOf("?");
    let queryString = "";
    let href = urlParts.href;
    if (queryIndex !== -1 && queryIndex < queryEnd) {
      queryString = urlParts.href.slice(queryIndex, queryEnd);
      href = urlParts.href.slice(0, queryIndex + 1) + urlParts.href.slice(queryEnd);
    }
    if (component === "href" || component === "search") {
      setQueryString(this.#urlSearchParams, queryString);
    }
    this.#href = href;
  }
}

/**
 * @param {unknown} value
 * @returns {string}
 */
function stringify(value) {
  if (typeof value === "symbol") {
    throw new TypeError("Cannot convert a Symbol value to a string");
  }
  // Unicode matching sees paired surrogates as one non-surrogate code point.
  return String(value).replace(/[\uD800-\uDFFF]/gu, "\uFFFD");
}
