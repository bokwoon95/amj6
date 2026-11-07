// Implemented Web Crypto subset only. No keys, encryption, signing, derivation,
// wrapping, or other SubtleCrypto operations are available.
declare module "amj:crypto" {
  export type IntegerArray = import("amj:crypto_impl").IntegerArray;
  export type DigestAlgorithm = import("amj:crypto_impl").DigestAlgorithm;

  export interface SubtleCrypto {
    /**
     * Supports SHA-1, SHA-256, SHA-384, and SHA-512, case-insensitively.
     * Snapshots the supplied bytes before returning; hashes on a native worker.
     * Rejects unsupported algorithms, invalid input, and native failures.
     * SharedArrayBuffer inputs are not supported.
     */
    digest(algorithm: string | { name: string }, data: ArrayBuffer | ArrayBufferView): Promise<ArrayBuffer>;
  }

  export interface Crypto {
    /**
     * Synchronously fills an integer typed array with OS-backed secure random
     * bytes and returns that same array. Maximum 65,536 bytes. Float arrays,
     * DataView, shared buffers, and immutable/detached buffers are not supported.
     */
    getRandomValues<ArrayType extends IntegerArray>(array: ArrayType): ArrayType;

    /** Returns a cryptographically random RFC 9562 version-4 UUID. */
    randomUUID(): string;

    readonly subtle: SubtleCrypto;
  }

  /** Validation failures use named Error objects where DOMException is absent. */
  export const crypto: Crypto;
}
