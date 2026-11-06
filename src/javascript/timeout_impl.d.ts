// JavaScript representation of the normalized operations in timeout_impl.hpp.
declare module "amj:timeout_impl" {
  /** callback receives no arguments; delay is an integer in [0, 2147483647]. Returns an ID in [1, 2147483647]. */
  export function timeoutSet(callback: () => void, delayMs: number): number;

  /** timerID is a nonnegative safe integer; unknown or cleared IDs have no effect. */
  export function timeoutClear(timerID: number): void;
}
