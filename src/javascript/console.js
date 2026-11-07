// @ts-check
// Text-only console facade. The host chooses where to install this export.
// No globals are modified; native consoleWrite supplies the output destination.
import { consoleWrite } from "amj:console_impl";

/**
 * Bounded textual inspection, not a live browser-devtools object reference.
 * Own accessors are described rather than invoked. Shared references are not
 * mistaken for cycles: only objects on the current ancestor path are tracked.
 * @param {unknown} value
 * @param {Set<object>} ancestors
 * @param {number} depth
 * @returns {string}
 */
function inspectConsoleValue(value, ancestors, depth) {
  if (typeof value === "string") {
    return JSON.stringify(value);
  }
  if (typeof value === "bigint") {
    return `${value}n`;
  }
  if (typeof value === "number" && Object.is(value, -0)) {
    return "-0";
  }
  if (typeof value === "function") {
    if (value.name) {
      return `[Function: ${value.name}]`;
    }
    return "[Function]";
  }
  if (value === null || typeof value !== "object") {
    return String(value);
  }
  if (value instanceof Error) {
    const stack = value.stack;
    if (typeof stack === "string") {
      return stack;
    }
    return String(value);
  }
  if (value instanceof Date) {
    return Date.prototype.toString.call(value);
  }
  if (value instanceof RegExp) {
    return RegExp.prototype.toString.call(value);
  }
  if (ancestors.has(value)) {
    return "[Circular]";
  }
  const isArray = Array.isArray(value);
  if (depth >= 4) {
    if (isArray) {
      return "[Array]";
    }
    return "[Object]";
  }
  ancestors.add(value);
  try {
    /** @type {Array<string>} */
    const parts = [];
    if (value instanceof Map) {
      for (const [key, entry] of value) {
        if (parts.length === 100) {
          parts.push("...");
          break;
        }
        parts.push(`${inspectConsoleValue(key, ancestors, depth + 1)} => ${inspectConsoleValue(entry, ancestors, depth + 1)}`);
      }
      return `Map(${value.size}) { ${parts.join(", ")} }`;
    }
    if (value instanceof Set) {
      for (const entry of value) {
        if (parts.length === 100) {
          parts.push("...");
          break;
        }
        parts.push(inspectConsoleValue(entry, ancestors, depth + 1));
      }
      return `Set(${value.size}) { ${parts.join(", ")} }`;
    }
    /** @type {Array<string | symbol>} */
    let keys = [];
    let truncated = false;
    if (isArray) {
      for (let index = 0; index < Math.min(value.length, 100); index++) {
        keys.push(String(index));
      }
      truncated = value.length > 100;
    } else {
      for (const key of Reflect.ownKeys(value)) {
        const descriptor = Object.getOwnPropertyDescriptor(value, key);
        if (!descriptor?.enumerable) {
          continue;
        }
        if (keys.length === 100) {
          truncated = true;
          break;
        }
        keys.push(key);
      }
    }
    for (const key of keys) {
      const descriptor = Object.getOwnPropertyDescriptor(value, key);
      let text = "<empty>";
      if (descriptor !== undefined) {
        if (Object.hasOwn(descriptor, "value")) {
          text = inspectConsoleValue(descriptor.value, ancestors, depth + 1);
        } else if (descriptor.get !== undefined && descriptor.set !== undefined) {
          text = "[Getter/Setter]";
        } else if (descriptor.get !== undefined) {
          text = "[Getter]";
        } else {
          text = "[Setter]";
        }
      }
      if (isArray) {
        parts.push(text);
      } else {
        let label = String(key);
        if (typeof key === "symbol") {
          label = `[${label}]`;
        } else if (!/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(key)) {
          label = JSON.stringify(key);
        }
        parts.push(`${label}: ${text}`);
      }
    }
    if (truncated) {
      parts.push("...");
    }
    if (isArray) {
      return `[${parts.join(", ")}]`;
    }
    return `{ ${parts.join(", ")} }`;
  } finally {
    ancestors.delete(value);
  }
}

/**
 * Supports %s, %d, %i, %f, %o, %O, %c, and %%. CSS from %c is consumed but not
 * rendered. Object formatting is deliberately text-only and bounded.
 * @param {Array<unknown>} data
 * @returns {string}
 */
function formatConsoleArguments(data) {
  let text = "";
  let nextArgument = 0;
  const first = data.at(0);
  if (typeof first === "string") {
    nextArgument = 1;
    for (let index = 0; index < first.length; index++) {
      const character = first.charAt(index);
      if (character !== "%" || data.length === 1) {
        text += character;
        continue;
      }
      const specifier = first.charAt(index + 1);
      if (specifier === "%") {
        text += "%";
        index++;
        continue;
      }
      if (nextArgument >= data.length || !["s", "d", "i", "f", "o", "O", "c"].includes(specifier)) {
        text += character;
        continue;
      }
      const value = data.at(nextArgument);
      nextArgument++;
      index++;
      if (specifier === "s") {
        text += String(value);
      } else if (specifier === "d" || specifier === "i" || specifier === "f") {
        if (typeof value === "symbol") {
          text += "NaN";
        } else if (typeof value === "bigint" && specifier !== "f") {
          text += `${value}n`;
        } else if (specifier === "i") {
          text += String(Number.parseInt(String(value), 10));
        } else if (specifier === "f") {
          text += String(Number.parseFloat(String(value)));
        } else {
          text += String(Number(value));
        }
      } else if (specifier === "o" || specifier === "O") {
        text += inspectConsoleValue(value, new Set(), 0);
      }
    }
  }
  for (let index = nextArgument; index < data.length; index++) {
    if (index > 0) {
      text += " ";
    }
    const value = data.at(index);
    if (typeof value === "string") {
      text += value;
    } else {
      text += inspectConsoleValue(value, new Set(), 0);
    }
  }
  return text;
}

/**
 * Currently implements the five text-output methods only. No clear, groups,
 * tables, counters, or timers are exposed. Each call emits one native message.
 */
export const console = {
  /** @param {...unknown} data */
  log(...data) {
    consoleWrite("log", formatConsoleArguments(data));
  },

  /** @param {...unknown} data */
  debug(...data) {
    consoleWrite("debug", formatConsoleArguments(data));
  },

  /** @param {...unknown} data */
  info(...data) {
    consoleWrite("info", formatConsoleArguments(data));
  },

  /** @param {...unknown} data */
  warn(...data) {
    consoleWrite("warn", formatConsoleArguments(data));
  },

  /** @param {...unknown} data */
  error(...data) {
    consoleWrite("error", formatConsoleArguments(data));
  },
};
