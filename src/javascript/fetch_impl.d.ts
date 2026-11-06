// JavaScript representation of the boundary types and operations in fetch_impl.hpp.
declare module "amj:fetch_impl" {
  export interface FetchRequestHandle {
    readonly __fetchRequestHandle: unique symbol;
  }
  export interface FetchFailure {
    type: string;
    message: string;
  }
  export interface FetchResponseMetadata {
    status: number;
    headers: [string, string][];
  }
  export interface FetchImpl {
    start(
      url: string,
      method: string,
      headerPairs: [string, string][],
      bytes: Uint8Array,
      onHeaders: (failure: FetchFailure | null, metadata: FetchResponseMetadata | null) => void,
    ): FetchRequestHandle;
    read(request: FetchRequestHandle, onRead: (failure: FetchFailure | null, chunk: Uint8Array | null) => void): void;
    close(request: FetchRequestHandle): void;
  }
  export const fetchImpl: FetchImpl;
}
