// JavaScript contract for console_impl.hpp. Formatting belongs to the JS facade.
declare module "amj:console_impl" {
  /**
   * Synchronously forwards one formatted message to the host's sink without
   * appending a newline. Returns undefined; invalid arguments or native failures
   * throw. No JavaScript values are retained after this call.
   */
  export function consoleWrite(
    level: "log" | "debug" | "info" | "warn" | "error",
    text: string,
  ): void;
}
