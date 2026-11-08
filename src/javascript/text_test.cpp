#include "javascript.hpp"

#include <gtest/gtest.h>
#include <quickjs.h>

#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>

namespace amj::javascript {
namespace {

class TextTest : public testing::Test {
  protected:
    JSRuntime* jsRuntime = nullptr;
    JSContext* jsContext = nullptr;
    std::string textSource = "";

    void SetUp() override {
        jsRuntime = JS_NewRuntime();
        ASSERT_NE(jsRuntime, nullptr);
        jsContext = JS_NewContext(jsRuntime);
        ASSERT_NE(jsContext, nullptr);

        std::ifstream sourceFile(AMJ_TEXT_SOURCE_PATH, std::ios::binary);
        ASSERT_TRUE(sourceFile.is_open()) << "Could not open " << AMJ_TEXT_SOURCE_PATH;
        textSource.assign(std::istreambuf_iterator<char>(sourceFile), std::istreambuf_iterator<char>());
        ASSERT_FALSE(sourceFile.bad()) << "Could not read " << AMJ_TEXT_SOURCE_PATH;
    }

    void TearDown() override {
        if (jsContext != nullptr) {
            JS_FreeContext(jsContext);
        }
        if (jsRuntime != nullptr) {
            JS_FreeRuntime(jsRuntime);
        }
    }

    testing::AssertionResult run(const std::string& assertions) {
        // Same-module assertions access the real exports without a module loader.
        const std::string source = textSource + "\n" + assertions;
        const OwnedJSValue result(jsContext, JS_Eval(jsContext, source.c_str(), source.size(), AMJ_TEXT_SOURCE_PATH, JS_EVAL_TYPE_MODULE));
        JSContext* exceptionContext = jsContext;
        JSValue exceptionValue = JS_UNDEFINED;
        if (JS_IsException(result.get())) {
            exceptionValue = JS_GetException(jsContext);
        } else {
            JSContext* jobContext = nullptr;
            int status = 0;
            do {
                status = JS_ExecutePendingJob(jsRuntime, &jobContext);
            } while (status > 0);
            if (status < 0) {
                if (jobContext == nullptr) {
                    return testing::AssertionFailure() << "QuickJS job failed without an exception context";
                }
                exceptionContext = jobContext;
                exceptionValue = JS_GetException(jobContext);
            } else {
                const JSPromiseStateEnum promiseState = JS_PromiseState(jsContext, result.get());
                if (promiseState == JS_PROMISE_FULFILLED) {
                    return testing::AssertionSuccess();
                }
                if (promiseState == JS_PROMISE_PENDING) {
                    return testing::AssertionFailure() << "Test module remained pending after draining QuickJS jobs";
                }
                if (promiseState != JS_PROMISE_REJECTED) {
                    return testing::AssertionFailure() << "Expected module evaluation to return a Promise";
                }
                JS_PromiseMarkAsHandled(jsContext, result.get());
                exceptionValue = JS_PromiseResult(jsContext, result.get());
            }
        }
        const OwnedJSValue exception(exceptionContext, exceptionValue);
        const std::optional<std::string> message = jsstring(exceptionContext, exception.get());
        std::string diagnostic = "JavaScript test failed";
        if (message.has_value()) {
            diagnostic += ": " + message.value();
        } else {
            const OwnedJSValue conversionException(exceptionContext, JS_GetException(exceptionContext));
        }
        if (JS_IsObject(exception.get())) {
            const OwnedJSValue stack(exceptionContext, JS_GetPropertyStr(exceptionContext, exception.get(), "stack"));
            if (JS_IsException(stack.get())) {
                const OwnedJSValue stackException(exceptionContext, JS_GetException(exceptionContext));
            } else if (JS_IsString(stack.get())) {
                const std::optional<std::string> stackText = jsstring(exceptionContext, stack.get());
                if (stackText.has_value()) {
                    diagnostic += "\n" + stackText.value();
                } else {
                    const OwnedJSValue conversionException(exceptionContext, JS_GetException(exceptionContext));
                }
            }
        }
        return testing::AssertionFailure() << diagnostic;
    }
};

TEST_F(TextTest, EncoderDefaultsAndCoercion) {
    ASSERT_TRUE(run(R"JS(
const encoder = new TextEncoder();
if (encoder.encoding !== "utf-8") {
  throw new Error("Wrong encoder encoding");
}
if (!(encoder.encode() instanceof Uint8Array)) {
  throw new Error("Expected Uint8Array");
}
if (encoder.encode().length !== 0 || encoder.encode(undefined).length !== 0) {
  throw new Error("Omitted and undefined input must encode as empty");
}
for (const [input, expected] of [[123, "123"], [null, "null"], [true, "true"]]) {
  if (new TextDecoder().decode(encoder.encode(input)) !== expected) {
    throw new Error(`Wrong coercion for ${input}`);
  }
}
let rejected = false;
try {
  encoder.encode(Symbol("input"));
} catch (error) {
  rejected = error instanceof TypeError;
}
if (!rejected) {
  throw new Error("Symbol input must throw TypeError");
}
)JS"));
}

TEST_F(TextTest, EncodesExactUtf8Bytes) {
    ASSERT_TRUE(run(R"JS(
const actual = Array.from(new TextEncoder().encode("A\0\u00a2\u20ac\u{1f30d}"));
const expected = [0x41, 0, 0xc2, 0xa2, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x8c, 0x8d];
if (JSON.stringify(actual) !== JSON.stringify(expected)) {
  throw new Error(`Wrong UTF-8 bytes: ${JSON.stringify(actual)}`);
}
)JS"));
}

TEST_F(TextTest, ReplacesLoneSurrogates) {
    ASSERT_TRUE(run(R"JS(
const encoder = new TextEncoder();
for (const input of ["\ud800", "\udfff"]) {
  const actual = Array.from(encoder.encode(input));
  if (JSON.stringify(actual) !== "[239,191,189]") {
    throw new Error("Wrong surrogate replacement");
  }
}
const actual = new TextDecoder().decode(encoder.encode("\ud800A\udfff"));
if (actual !== "\ufffdA\ufffd") {
  throw new Error("Lost characters around lone surrogates");
}
)JS"));
}

TEST_F(TextTest, DecoderLabelsAndOptions) {
    ASSERT_TRUE(run(R"JS(
for (const label of [undefined, "utf-8", " UTF8 \t", "unicode-1-1-utf-8"]) {
  const decoder = new TextDecoder(label);
  if (decoder.encoding !== "utf-8" || decoder.fatal || decoder.ignoreBOM) {
    throw new Error("Wrong decoder defaults");
  }
}
const decoder = new TextDecoder("utf-8", {fatal: true, ignoreBOM: true});
if (!decoder.fatal || !decoder.ignoreBOM) {
  throw new Error("Options not preserved");
}
for (const label of ["latin1", "", Symbol("label")]) {
  let rejected = false;
  try {
    new TextDecoder(label);
  } catch (error) {
    rejected = error instanceof (typeof label === "symbol" ? TypeError : RangeError);
  }
  if (!rejected) {
    throw new Error("Invalid decoder label accepted");
  }
}
)JS"));
}

TEST_F(TextTest, DecodesBuffersAndViewSlices) {
    ASSERT_TRUE(run(R"JS(
const decoder = new TextDecoder();
if (decoder.decode() !== "") {
  throw new Error("Expected empty decoding");
}
const bytes = new Uint8Array([0x58, 0x41, 0xc2, 0xa2, 0x59]);
for (const input of [bytes.subarray(1, 4), new DataView(bytes.buffer, 1, 3), bytes.buffer.slice(1, 4)]) {
  if (decoder.decode(input) !== "A\u00a2") {
    throw new Error("Ignored view offset or length");
  }
}
for (const input of [null, "text", [65], {}]) {
  let rejected = false;
  try {
    decoder.decode(input);
  } catch (error) {
    rejected = error instanceof TypeError;
  }
  if (!rejected) {
    throw new Error("Non-buffer input accepted");
  }
}
)JS"));
}

TEST_F(TextTest, DecodesUnicodeBoundaries) {
    ASSERT_TRUE(run(R"JS(
const bytes = new Uint8Array([
  0x7f, 0xc2, 0x80, 0xdf, 0xbf, 0xe0, 0xa0, 0x80,
  0xed, 0x9f, 0xbf, 0xee, 0x80, 0x80, 0xef, 0xbf, 0xbf,
  0xf0, 0x90, 0x80, 0x80, 0xf4, 0x8f, 0xbf, 0xbf
]);
const expected = "\u007f\u0080\u07ff\u0800\ud7ff\ue000\uffff\u{10000}\u{10ffff}";
if (new TextDecoder().decode(bytes) !== expected) {
  throw new Error("Wrong Unicode boundary decoding");
}
)JS"));
}

TEST_F(TextTest, ReplacesMalformedUtf8) {
    ASSERT_TRUE(run(R"JS(
const cases = [
  [[0x80], "\ufffd"],
  [[0xc0, 0xaf], "\ufffd\ufffd"],
  [[0xe0, 0x80, 0x80], "\ufffd\ufffd\ufffd"],
  [[0xed, 0xa0, 0x80], "\ufffd\ufffd\ufffd"],
  [[0xf4, 0x90, 0x80, 0x80], "\ufffd\ufffd\ufffd\ufffd"],
  [[0xff], "\ufffd"],
  [[0xe2, 0x82], "\ufffd"],
  [[0xe2, 0x41], "\ufffdA"]
];
for (const [bytes, expected] of cases) {
  const actual = new TextDecoder().decode(new Uint8Array(bytes));
  if (actual !== expected) {
    throw new Error(`Wrong replacement for ${bytes}: ${JSON.stringify(actual)}`);
  }
}
)JS"));
}

TEST_F(TextTest, FatalDecodingRejectsAndRecovers) {
    ASSERT_TRUE(run(R"JS(
const decoder = new TextDecoder("utf-8", {fatal: true});
for (const bytes of [[0x80], [0xc0, 0xaf], [0xed, 0xa0, 0x80], [0xf4, 0x90, 0x80, 0x80], [0xe2, 0x82]]) {
  let rejected = false;
  try {
    decoder.decode(new Uint8Array(bytes));
  } catch (error) {
    rejected = error instanceof TypeError;
  }
  if (!rejected) {
    throw new Error(`Fatal decoder accepted ${bytes}`);
  }
  if (decoder.decode(new Uint8Array([65])) !== "A") {
    throw new Error("Decoder failed to recover");
  }
}
)JS"));
}

TEST_F(TextTest, BomHandlingAndReset) {
    ASSERT_TRUE(run(R"JS(
const bytes = new Uint8Array([0xef, 0xbb, 0xbf, 65, 0xef, 0xbb, 0xbf]);
const decoder = new TextDecoder();
for (let index = 0; index < 2; index++) {
  if (decoder.decode(bytes) !== "A\ufeff") {
    throw new Error("BOM stripping or reset failed");
  }
}
const preserving = new TextDecoder("utf-8", {ignoreBOM: true});
if (preserving.decode(bytes) !== "\ufeffA\ufeff") {
  throw new Error("ignoreBOM should preserve BOM");
}
)JS"));
}

TEST_F(TextTest, IncrementalDecodingAtEverySplit) {
    ASSERT_TRUE(run(R"JS(
const expected = "A\u00a2\u20ac\u{1f30d}Z";
const bytes = new Uint8Array([65, 0xc2, 0xa2, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x8c, 0x8d, 90]);
for (let split = 0; split <= bytes.length; split++) {
  const decoder = new TextDecoder();
  const actual = decoder.decode(bytes.subarray(0, split), {stream: true})
    + decoder.decode(bytes.subarray(split), {stream: true}) + decoder.decode();
  if (actual !== expected) {
    throw new Error(`Wrong incremental decoding at split ${split}`);
  }
}
const decoder = new TextDecoder();
let actual = "";
for (const byte of bytes) {
  actual += decoder.decode(new Uint8Array([byte]), {stream: true});
}
actual += decoder.decode();
if (actual !== expected) {
  throw new Error("Byte-at-a-time decoding failed");
}
)JS"));
}

TEST_F(TextTest, IncrementalBomAndTruncation) {
    ASSERT_TRUE(run(R"JS(
const decoder = new TextDecoder();
let actual = decoder.decode(new Uint8Array([0xef]), {stream: true});
actual += decoder.decode(new Uint8Array([0xbb]), {stream: true});
actual += decoder.decode(new Uint8Array([0xbf, 65]), {stream: true});
actual += decoder.decode();
if (actual !== "A") {
  throw new Error("Split BOM was not stripped");
}
if (decoder.decode(new Uint8Array([0xe2, 0x82]), {stream: true}) !== "") {
  throw new Error("Incomplete sequence emitted prematurely");
}
if (decoder.decode() !== "\ufffd" || decoder.decode() !== "") {
  throw new Error("Incomplete sequence flush did not reset state");
}
const fatal = new TextDecoder("utf-8", {fatal: true});
fatal.decode(new Uint8Array([0xf0, 0x9f]), {stream: true});
let rejected = false;
try {
  fatal.decode();
} catch (error) {
  rejected = error instanceof TypeError;
}
if (!rejected) {
  throw new Error("Fatal decoder accepted a truncated stream");
}
)JS"));
}

TEST_F(TextTest, ModuleHarnessDrainsPromiseJobs) {
    ASSERT_TRUE(run(R"JS(
await Promise.resolve();
if (new TextDecoder().decode(new TextEncoder().encode("after await")) !== "after await") {
  throw new Error("Test did not resume after await");
}
)JS"));
}

TEST_F(TextTest, ModuleHarnessReportsRejectedPromises) {
    const testing::AssertionResult result = run(R"JS(
await Promise.resolve();
throw new Error("expected module rejection");
)JS");
    EXPECT_FALSE(result);
    EXPECT_NE(std::string(result.message()).find("expected module rejection"), std::string::npos);
    EXPECT_NE(std::string(result.message()).find("text.js"), std::string::npos);
}

TEST_F(TextTest, ModuleHarnessReportsSyntaxErrors) {
    const testing::AssertionResult result = run("const = ;");
    EXPECT_FALSE(result);
    EXPECT_NE(std::string(result.message()).find("SyntaxError"), std::string::npos);
}

} // namespace
} // namespace amj::javascript
