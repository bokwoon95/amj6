// @ts-check
// UTF-8 subset of TextEncoder/TextDecoder, including incremental decoding.
// No other polyfills or native bindings are required. Globals are not modified.

export class TextEncoder {
  /**
   * @returns {"utf-8"}
   */
  get encoding() {
    return "utf-8";
  }

  /**
   * @param {unknown} [input]
   * @returns {Uint8Array}
   */
  encode(input) {
    if (typeof input === "symbol") {
      throw new TypeError("Cannot encode a Symbol");
    }
    let text = "";
    if (input !== undefined) {
      text = String(input);
    }
    let length = 0;
    for (const character of text) {
      const point = character.codePointAt(0);
      if (point === undefined) {
        throw new TypeError("Unexpected empty string character");
      }
      if (point < 0x80) {
        length++;
      } else if (point < 0x800) {
        length += 2;
      } else if (point < 0x10000) {
        length += 3;
      } else {
        length += 4;
      }
    }
    const bytes = new Uint8Array(length);
    let offset = 0;
    for (const character of text) {
      let point = character.codePointAt(0);
      if (point === undefined) {
        throw new TypeError("Unexpected empty string character");
      }
      if (point >= 0xd800 && point <= 0xdfff) {
        point = 0xfffd;
      }
      if (point < 0x80) {
        bytes[offset++] = point;
      } else if (point < 0x800) {
        bytes[offset++] = 0xc0 | (point >> 6);
        bytes[offset++] = 0x80 | (point & 63);
      } else if (point < 0x10000) {
        bytes[offset++] = 0xe0 | (point >> 12);
        bytes[offset++] = 0x80 | ((point >> 6) & 63);
        bytes[offset++] = 0x80 | (point & 63);
      } else {
        bytes[offset++] = 0xf0 | (point >> 18);
        bytes[offset++] = 0x80 | ((point >> 12) & 63);
        bytes[offset++] = 0x80 | ((point >> 6) & 63);
        bytes[offset++] = 0x80 | (point & 63);
      }
    }
    return bytes;
  }
}

export class TextDecoder {
  #fatal = false;
  #ignoreBOM = false;
  #needed = 0;
  #seen = 0;
  #point = 0;
  #lower = 0x80;
  #upper = 0xbf;
  #bomSeen = false;

  /**
   * @param {unknown} [label]
   * @param {{fatal?: unknown, ignoreBOM?: unknown} | null } [options]
   */
  constructor(label, options) {
    if (typeof label === "symbol") {
      throw new TypeError("Decoder label cannot be a Symbol");
    }
    if (label !== undefined && !["utf-8", "utf8", "unicode-1-1-utf-8"].includes(String(label).trim().toLowerCase())) {
      throw new RangeError("Only UTF-8 decoding is supported");
    }
    if (options !== undefined && options !== null) {
      this.#fatal = Boolean(options.fatal);
      this.#ignoreBOM = Boolean(options.ignoreBOM);
    }
  }

  /**
   * @returns {"utf-8"}
   */
  get encoding() {
    return "utf-8";
  }

  get fatal() {
    return this.#fatal;
  }

  get ignoreBOM() {
    return this.#ignoreBOM;
  }

  /**
   * @param {ArrayBuffer | ArrayBufferView} [input]
   * @param {{stream?: unknown} | null} [options]
   * @returns {string}
   */
  decode(input, options) {
    let bytes = new Uint8Array(0);
    if (input instanceof ArrayBuffer) {
      bytes = new Uint8Array(input);
    } else if (input !== undefined) {
      if (!ArrayBuffer.isView(input)) {
        throw new TypeError("Expected an ArrayBuffer or view");
      }
      bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
    }

    /**
     * @type {Array<string>}
     */
    const characters = [];

    /**
     * @param {number} point
     * @returns {void}
     */
    const emit = (point) => {
      if (!this.#bomSeen) {
        this.#bomSeen = true;
        if (point === 0xfeff && !this.#ignoreBOM) {
          return;
        }
      }
      characters.push(String.fromCodePoint(point));
    };

    const invalid = () => {
      this.#needed = 0;
      this.#seen = 0;
      this.#point = 0;
      this.#lower = 0x80;
      this.#upper = 0xbf;
      if (this.#fatal) {
        this.#bomSeen = false;
        throw new TypeError("Invalid UTF-8 data");
      }
      emit(0xfffd);
    };

    for (let index = 0; index < bytes.length; index++) {
      const byte = bytes[index];
      if (byte === undefined) {
        throw new RangeError("Invalid UTF-8 byte index");
      }
      if (this.#needed === 0) {
        if (byte <= 0x7f) {
          emit(byte);
        } else if (byte >= 0xc2 && byte <= 0xdf) {
          this.#needed = 1;
          this.#point = byte & 31;
        } else if (byte >= 0xe0 && byte <= 0xef) {
          this.#needed = 2;
          this.#point = byte & 15;
          if (byte === 0xe0) {
            this.#lower = 0xa0;
          }
          if (byte === 0xed) {
            this.#upper = 0x9f;
          }
        } else if (byte >= 0xf0 && byte <= 0xf4) {
          this.#needed = 3;
          this.#point = byte & 7;
          if (byte === 0xf0) {
            this.#lower = 0x90;
          }
          if (byte === 0xf4) {
            this.#upper = 0x8f;
          }
        } else {
          invalid();
        }
      } else if (byte < this.#lower || byte > this.#upper) {
        invalid();
        index--; // Reconsume an invalid continuation as a new lead.
      } else {
        this.#lower = 0x80;
        this.#upper = 0xbf;
        this.#point = (this.#point << 6) | (byte & 63);
        this.#seen++;
        if (this.#seen === this.#needed) {
          emit(this.#point);
          this.#needed = 0;
          this.#seen = 0;
          this.#point = 0;
        }
      }
    }
    if (!options || !options.stream) {
      if (this.#needed !== 0) {
        invalid();
      }
      this.#bomSeen = false;
    }
    return characters.join("");
  }
}
