#pragma once

#include <quickjs.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace amj::javascript {

enum class ConsoleLevel : std::uint8_t {
    Log,
    Debug,
    Info,
    Warn,
    Error,
};

struct ConsoleData {
    ConsoleLevel level = {};
    std::string text = "";
};

// JS arguments: level (log/debug/info/warn/error) and already-formatted text.
// Returns undefined, or JS_EXCEPTION. Borrows the ConsoleSink carrier retained
// in functionData[0] by the module's JS_NewCFunctionData2 binding.
JSValue consoleWrite(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int magic, JSValueConst* functionData);

// Registers amj:console_impl, exporting consoleWrite(level, text).
// Accepts only the levels log/debug/info/warn/error and already-formatted text;
// forwards one owned message per call without adding a newline or formatting.
//
// Takes ownership of consoleSink; the ConsoleSink carrier deletes it at finalization.
// Registration failure also releases ownership through cleanup.
// Captures must contain only native data, not untracked JS references. Dependencies must
// remain valid through callback destruction, potentially during runtime teardown.
// consoleSink must be non-null and contain a nonempty function. Its callback must
// return promptly and not throw, execute JS, or destroy
// the context/runtime. Calls and callback destruction occur on the JS owner
// thread. The host may enqueue messages for delivery on another thread.
// Unexpected C++ exceptions are converted to pending JS exceptions.
//
// Call once per context before loading consumers. Returns an engine-managed
// module, or nullptr with a pending exception. No globals are installed.
JSModuleDef* createConsoleImplModule(JSContext* jsContext, std::unique_ptr<std::function<void(ConsoleData)>> consoleSink);

} // namespace amj::javascript
