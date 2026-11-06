// The host copies these module exports onto the runtime's global object.
// This file supplies types only; it does not install or execute anything.
declare var TextEncoder: typeof import("amj:text").TextEncoder;
type TextEncoder = import("amj:text").TextEncoder;

declare var TextDecoder: typeof import("amj:text").TextDecoder;
type TextDecoder = import("amj:text").TextDecoder;

declare var URL: typeof import("amj:url").URL;
type URL = import("amj:url").URL;

declare var URLSearchParams: typeof import("amj:url").URLSearchParams;
type URLSearchParams = import("amj:url").URLSearchParams;

declare var Headers: typeof import("amj:fetch").Headers;
type Headers = import("amj:fetch").Headers;

declare var fetch: typeof import("amj:fetch").fetch;

declare var setTimeout: typeof import("amj:timeout").setTimeout;

declare var clearTimeout: typeof import("amj:timeout").clearTimeout;
