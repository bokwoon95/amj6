// @ts-check
// Web Crypto subset: synchronous OS randomness and worker-backed SHA digests.
// No globals are modified. See crypto.d.ts for the complete supported surface.
import { cryptoDigest, cryptoGetRandomValues } from "amj:crypto_impl";

/** @type {import("amj:crypto").Crypto} */
export const crypto = {
  getRandomValues(array) {
    if (arguments.length === 0) {
      throw new TypeError("getRandomValues requires an integer typed array");
    }
    return cryptoGetRandomValues(array);
  },

  randomUUID() {
    const bytes = cryptoGetRandomValues(new Uint8Array(16));
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    view.setUint8(6, (view.getUint8(6) & 0x0f) | 0x40);
    view.setUint8(8, (view.getUint8(8) & 0x3f) | 0x80);
    let hex = "";
    for (const byte of bytes) {
      hex += byte.toString(16).padStart(2, "0");
    }
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
  },

  get subtle() {
    return subtle;
  },
};

/** @type {import("amj:crypto").SubtleCrypto} */
const subtle = {
  async digest(algorithm, data) {
    if (arguments.length < 2) {
      throw new TypeError("digest requires an algorithm and input data");
    }
    /** @type {unknown} */
    let name = algorithm;
    if (typeof algorithm === "object" && algorithm !== null) {
      name = algorithm.name;
      if (name === undefined) {
        throw new TypeError("Digest algorithm requires a name");
      }
    }
    if (typeof name === "symbol") {
      throw new TypeError("Digest algorithm name cannot be a Symbol");
    }
    const normalized = String(name).toUpperCase();
    if (normalized !== "SHA-1" && normalized !== "SHA-256" && normalized !== "SHA-384" && normalized !== "SHA-512") {
      const error = new Error(`Unsupported digest algorithm: ${normalized}`);
      error.name = "NotSupportedError";
      throw error;
    }
    /** @type {Uint8Array} */
    let bytes;
    if (data instanceof ArrayBuffer) {
      bytes = new Uint8Array(data);
    } else if (ArrayBuffer.isView(data)) {
      if (!(data.buffer instanceof ArrayBuffer)) {
        throw new TypeError("Digest input must not use a shared buffer");
      }
      if (!(data instanceof DataView)) {
        // Validate detached/out-of-bounds typed arrays before normalizing their
        // byte range. Their byteOffset/byteLength alone can misleadingly be zero.
        Uint8Array.prototype.values.call(data);
      }
      bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    } else {
      throw new TypeError("Digest input must be an ArrayBuffer or buffer view");
    }
    return new Promise((resolve, reject) => {
      // Native code copies this slice synchronously, before returning from
      // submission. Workers never retain a pointer into JS-owned storage.
      cryptoDigest(normalized, bytes, (error, result) => {
        if (error !== null) {
          reject(error);
        } else if (result === null) {
          reject(new Error("Native digest returned no result"));
        } else {
          resolve(result);
        }
      });
    });
  },
};
