// Normalized native contract, not the public browser fetch surface.
declare module "amj:fetch_impl" {
  export interface FetchRequest {
    url: string;
    method: string;
    headers: Array<[string, string]>;
    body: Uint8Array;
  }

  export interface FetchResponse {
    /** Resolves after final response headers, including redirects; rejects on failure. */
    readonly ready: Promise<void>;

    /** Access before ready or after close throws. Headers are returned as a copy. */
    readonly status: number;
    readonly headers: Array<[string, string]>;
    readonly url: string;

    /** Await ready first. One outstanding read; nonempty bytes or null for EOF. */
    read(): Promise<Uint8Array | null>;

    /** Idempotent. Frees native state and rejects pending ready/read operations. */
    close(): void;
  }

  export interface FetchClient {
    /** Copies normalized request fields and upload bytes before returning. */
    send(fetchRequest: FetchRequest): FetchResponse;
  }

  export const fetchClient: FetchClient;
}
