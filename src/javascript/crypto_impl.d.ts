// Normalized native operations implemented by crypto_impl.hpp/.cpp.
declare module "amj:crypto_impl" {
  export type IntegerArray =
    | Int8Array
    | Uint8Array
    | Uint8ClampedArray
    | Int16Array
    | Uint16Array
    | Int32Array
    | Uint32Array
    | BigInt64Array
    | BigUint64Array;

  export type DigestAlgorithm = "SHA-1" | "SHA-256" | "SHA-384" | "SHA-512";

  /** Synchronous; fills and returns the same array. Maximum 65,536 bytes. */
  export function cryptoGetRandomValues<ArrayType extends IntegerArray>(array: ArrayType): ArrayType;

  /**
   * Copies input before returning, then hashes on a libuv worker. Completion
   * runs on the JS thread. Submission errors throw; worker errors are delivered
   * to the callback. Shutdown releases callbacks without invoking them.
   */
  export function cryptoDigest(
    algorithm: DigestAlgorithm,
    data: Uint8Array,
    completion: (error: unknown, result: ArrayBuffer | null) => void,
  ): void;
}
