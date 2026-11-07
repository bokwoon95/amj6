// Direct native module; no JavaScript facade is required.
declare module "amj:microtask" {
  /**
   * Runs callback with no arguments at a host microtask checkpoint, after the
   * current JavaScript execution. Its return value is discarded. Exceptions
   * are reported by the host, not converted into Promise rejections.
   */
  export function queueMicrotask(callback: () => void): void;
}
