// The host copies these module exports onto the runtime's global object.
// This file supplies types only; it does not install or execute anything.
declare var AbortController: typeof import("amj:abort").AbortController;
type AbortController = import("amj:abort").AbortController;

declare var AbortSignal: typeof import("amj:abort").AbortSignal;
type AbortSignal = import("amj:abort").AbortSignal;

declare var console: typeof import("amj:console").console;

declare var crypto: typeof import("amj:crypto").crypto;
type Crypto = import("amj:crypto").Crypto;
type SubtleCrypto = import("amj:crypto").SubtleCrypto;

declare var Event: typeof import("amj:event").Event;
type Event = import("amj:event").Event;

declare var CustomEvent: typeof import("amj:event").CustomEvent;
type CustomEvent<Detail = unknown> = import("amj:event").CustomEvent<Detail>;

declare var EventTarget: typeof import("amj:event").EventTarget;
type EventTarget = import("amj:event").EventTarget;

declare var fetch: typeof import("amj:fetch").fetch;

declare var Headers: typeof import("amj:fetch").Headers;
type Headers = import("amj:fetch").Headers;

declare var TextDecoder: typeof import("amj:text").TextDecoder;
type TextDecoder = import("amj:text").TextDecoder;

declare var TextEncoder: typeof import("amj:text").TextEncoder;
type TextEncoder = import("amj:text").TextEncoder;

declare var clearTimeout: typeof import("amj:timeout").clearTimeout;

declare var setTimeout: typeof import("amj:timeout").setTimeout;

declare var URL: typeof import("amj:url").URL;
type URL = import("amj:url").URL;

declare var URLSearchParams: typeof import("amj:url").URLSearchParams;
type URLSearchParams = import("amj:url").URLSearchParams;
