#include "microtask.hpp"

#include "javascript.hpp"

#include <gtest/gtest.h>
#include <quickjs.h>

#include <optional>
#include <string>

namespace amj::javascript {
namespace {

class MicrotaskTest : public testing::Test {
  protected:
    JSRuntime* jsRuntime = nullptr;
    JSContext* jsContext = nullptr;
    bool callbackFinalized = false;

    void SetUp() override {
        jsRuntime = JS_NewRuntime();
        ASSERT_NE(jsRuntime, nullptr);
        jsContext = JS_NewContext(jsRuntime);
        ASSERT_NE(jsContext, nullptr);
        ASSERT_NE(createMicrotaskModule(jsContext), nullptr);

        // Exercise the registered export, then install it as an embedding host would.
        const std::string source = R"JS(
import { queueMicrotask } from "amj:microtask";
globalThis.queueMicrotask = queueMicrotask;
)JS";
        const ScopedJSValue result(jsContext, JS_Eval(jsContext, source.c_str(), source.size(), "microtask_setup.js", JS_EVAL_TYPE_MODULE));
        ASSERT_FALSE(JS_IsException(result.get()));
        JSContext* jobContext = nullptr;
        int status = 0;
        do {
            status = JS_ExecutePendingJob(jsRuntime, &jobContext);
        } while (status > 0);
        ASSERT_EQ(status, 0);
        ASSERT_EQ(JS_PromiseState(jsContext, result.get()), JS_PROMISE_FULFILLED);
        ASSERT_FALSE(JS_IsJobPending(jsRuntime));
    }

    void TearDown() override {
        if (jsContext != nullptr) {
            JS_FreeContext(jsContext);
        }
        if (jsRuntime != nullptr) {
            JS_FreeRuntime(jsRuntime);
        }
    }

    // Intentionally does not drain jobs: each test controls its own checkpoints.
    testing::AssertionResult run(const std::string& source) {
        const ScopedJSValue result(jsContext, JS_Eval(jsContext, source.c_str(), source.size(), "microtask_test.js", JS_EVAL_TYPE_GLOBAL));
        if (!JS_IsException(result.get())) {
            return testing::AssertionSuccess();
        }
        const ScopedJSValue exception(jsContext, JS_GetException(jsContext));
        const std::optional<std::string> message = jsstring(jsContext, exception.get());
        if (message.has_value()) {
            return testing::AssertionFailure() << message.value();
        }
        const ScopedJSValue conversionException(jsContext, JS_GetException(jsContext));
        return testing::AssertionFailure() << "JavaScript test failed; exception could not be converted to text";
    }
};

TEST_F(MicrotaskTest, ExportsNativeFunction) {
    ASSERT_TRUE(run(R"JS(
if (typeof queueMicrotask !== "function" || queueMicrotask.name !== "queueMicrotask" || queueMicrotask.length !== 1) {
  throw new Error("Wrong native export");
}
)JS"));
    EXPECT_FALSE(JS_IsJobPending(jsRuntime));
}

TEST_F(MicrotaskTest, RejectsInvalidCallbacksWithoutEnqueueing) {
    ASSERT_TRUE(run(R"JS(
let rejected = false;
try {
  queueMicrotask();
} catch (error) {
  rejected = error instanceof TypeError;
}
if (!rejected) {
  throw new Error("Missing callback was accepted");
}
for (const callback of [undefined, null, false, 1, "callback", Symbol("callback"), {}, {handleEvent() {}}]) {
  rejected = false;
  try {
    queueMicrotask(callback);
  } catch (error) {
    rejected = error instanceof TypeError;
  }
  if (!rejected) {
    throw new Error("Non-callable callback was accepted");
  }
}
)JS"));
    EXPECT_FALSE(JS_IsJobPending(jsRuntime));
    EXPECT_FALSE(JS_HasException(jsContext));
}

TEST_F(MicrotaskTest, DefersInvocationAndPassesNoArguments) {
    ASSERT_TRUE(run(R"JS(
"use strict";
globalThis.order = ["start"];
const result = queueMicrotask(function () {
  if (arguments.length !== 0 || this !== undefined) {
    throw new Error("Unexpected callback arguments or receiver");
  }
  order.push("microtask");
}, "ignored");
order.push("end");
if (result !== undefined || JSON.stringify(order) !== '["start","end"]') {
  throw new Error("Callback was not deferred or return value was wrong");
}
)JS"));
    ASSERT_TRUE(JS_IsJobPending(jsRuntime));
    JSContext* jobContext = nullptr;
    ASSERT_EQ(JS_ExecutePendingJob(jsRuntime, &jobContext), 1);
    EXPECT_EQ(jobContext, jsContext);
    ASSERT_TRUE(run(R"JS(
if (JSON.stringify(order) !== '["start","end","microtask"]') {
  throw new Error("Queued callback did not run");
}
)JS"));
    EXPECT_FALSE(JS_IsJobPending(jsRuntime));
    EXPECT_EQ(JS_ExecutePendingJob(jsRuntime, &jobContext), 0);
}

TEST_F(MicrotaskTest, ExecutesCallbacksInFifoOrder) {
    ASSERT_TRUE(run(R"JS(
globalThis.order = [];
queueMicrotask(() => order.push(1));
queueMicrotask(() => order.push(2));
queueMicrotask(() => order.push(3));
)JS"));
    JSContext* jobContext = nullptr;
    for (int index = 1; index <= 3; index++) {
        ASSERT_EQ(JS_ExecutePendingJob(jsRuntime, &jobContext), 1);
        EXPECT_EQ(jobContext, jsContext);
        ASSERT_TRUE(run("if (order.length !== " + std::to_string(index) + ") {\n  throw new Error('Expected exactly one callback per job');\n}"));
    }
    ASSERT_TRUE(run(R"JS(
if (JSON.stringify(order) !== "[1,2,3]") {
  throw new Error("Wrong FIFO order");
}
)JS"));
    EXPECT_FALSE(JS_IsJobPending(jsRuntime));
}

TEST_F(MicrotaskTest, NestedCallbacksJoinTheEndOfTheQueue) {
    ASSERT_TRUE(run(R"JS(
globalThis.order = [];
queueMicrotask(() => {
  order.push("first");
  queueMicrotask(() => order.push("nested"));
});
queueMicrotask(() => order.push("second"));
)JS"));
    JSContext* jobContext = nullptr;
    for (int index = 0; index < 3; index++) {
        ASSERT_EQ(JS_ExecutePendingJob(jsRuntime, &jobContext), 1);
    }
    ASSERT_TRUE(run(R"JS(
if (JSON.stringify(order) !== '["first","second","nested"]') {
  throw new Error("Nested callback ran out of order");
}
)JS"));
    EXPECT_FALSE(JS_IsJobPending(jsRuntime));
}

TEST_F(MicrotaskTest, SharesQueueWithPromiseReactions) {
    ASSERT_TRUE(run(R"JS(
globalThis.order = [];
Promise.resolve().then(() => order.push("promise-first"));
queueMicrotask(() => order.push("microtask"));
Promise.resolve().then(() => order.push("promise-last"));
)JS"));
    JSContext* jobContext = nullptr;
    for (int index = 0; index < 3; index++) {
        ASSERT_EQ(JS_ExecutePendingJob(jsRuntime, &jobContext), 1);
    }
    ASSERT_TRUE(run(R"JS(
if (JSON.stringify(order) !== '["promise-first","microtask","promise-last"]') {
  throw new Error("Microtasks and Promise reactions did not share FIFO order");
}
)JS"));
    EXPECT_FALSE(JS_IsJobPending(jsRuntime));
}

TEST_F(MicrotaskTest, ReportsCallbackExceptionAndKeepsRemainingJobs) {
    ASSERT_TRUE(run(R"JS(
globalThis.expectedError = new Error("microtask failure");
globalThis.didRun = false;
queueMicrotask(() => {
  throw expectedError;
});
queueMicrotask(() => {
  didRun = true;
});
)JS"));
    JSContext* jobContext = nullptr;
    ASSERT_EQ(JS_ExecutePendingJob(jsRuntime, &jobContext), -1);
    ASSERT_EQ(jobContext, jsContext);
    {
        const ScopedJSValue exception(jobContext, JS_GetException(jobContext));
        const ScopedJSValue global(jsContext, JS_GetGlobalObject(jsContext));
        const ScopedJSValue expected(jsContext, JS_GetPropertyStr(jsContext, global.get(), "expectedError"));
        ASSERT_FALSE(JS_IsException(expected.get()));
        EXPECT_TRUE(JS_IsStrictEqual(jsContext, exception.get(), expected.get()));
    }
    EXPECT_FALSE(JS_HasException(jsContext));
    EXPECT_TRUE(JS_IsJobPending(jsRuntime));
    ASSERT_EQ(JS_ExecutePendingJob(jsRuntime, &jobContext), 1);
    ASSERT_TRUE(run(R"JS(
if (!didRun) {
  throw new Error("Remaining microtask was abandoned after an exception");
}
)JS"));
    EXPECT_FALSE(JS_IsJobPending(jsRuntime));
}

TEST_F(MicrotaskTest, DiscardsCallbackReturnValue) {
    ASSERT_TRUE(run(R"JS(
queueMicrotask(() => ({then() {
  throw new Error("A microtask return value must not be assimilated");
}}));
)JS"));
    JSContext* jobContext = nullptr;
    ASSERT_EQ(JS_ExecutePendingJob(jsRuntime, &jobContext), 1);
    EXPECT_FALSE(JS_HasException(jsContext));
    EXPECT_FALSE(JS_IsJobPending(jsRuntime));
}

TEST_F(MicrotaskTest, RetainsCallbackUntilExecution) {
    ASSERT_TRUE(run(R"JS(
globalThis.retainedValue = "";
(() => {
  const captured = {text: "still alive"};
  queueMicrotask(() => {
    retainedValue = captured.text;
  });
})();
)JS"));
    JS_RunGC(jsRuntime);
    JSContext* jobContext = nullptr;
    ASSERT_EQ(JS_ExecutePendingJob(jsRuntime, &jobContext), 1);
    ASSERT_TRUE(run(R"JS(
if (retainedValue !== "still alive") {
  throw new Error("Queued callback or its captures were lost");
}
)JS"));
    EXPECT_FALSE(JS_IsJobPending(jsRuntime));
}

TEST_F(MicrotaskTest, EnqueueAllocationFailureLeavesNoJob) {
    const ScopedJSValue callback(jsContext, JS_NewCFunction(jsContext, [](JSContext* /* jsContext */, JSValueConst /* thisValue */, int /* argc */, JSValueConst* /* argv */) -> JSValue {
        return JS_UNDEFINED;
    }, "callback", 0));
    ASSERT_FALSE(JS_IsException(callback.get()));
    JSValueConst callbackValue = callback.get();
    JS_SetMemoryLimit(jsRuntime, 1);
    const ScopedJSValue result(jsContext, queueMicrotask(jsContext, JS_UNDEFINED, 1, &callbackValue));
    JS_SetMemoryLimit(jsRuntime, 0); // QuickJS-NG uses zero for an unlimited heap.
    EXPECT_TRUE(JS_IsException(result.get()));
    EXPECT_TRUE(JS_HasException(jsContext));
    const ScopedJSValue exception(jsContext, JS_GetException(jsContext));
    EXPECT_FALSE(JS_IsJobPending(jsRuntime));
}

TEST_F(MicrotaskTest, ReleasesPendingCallbackOnRuntimeDestruction) {
    {
        JSClassID classID = 0;
        JS_NewClassID(jsRuntime, &classID);
        const JSClassDef jsClassDef = {
            .class_name = "MicrotaskLifetimeProbe",
            .finalizer = [](JSRuntime* /* jsRuntime */, JSValue value) {
                bool* finalized = static_cast<bool*>(JS_GetOpaque(value, JS_GetClassID(value)));
                if (finalized != nullptr) {
                    *finalized = true;
                }
            },
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        ASSERT_EQ(JS_NewClass(jsRuntime, classID, &jsClassDef), 0);
        const ScopedJSValue probe(jsContext, JS_NewObjectProtoClass(jsContext, JS_NULL, classID));
        ASSERT_FALSE(JS_IsException(probe.get()));
        ASSERT_EQ(JS_SetOpaque(probe.get(), &callbackFinalized), 0);
        JSValueConst functionData = probe.get();
        const ScopedJSValue callback(jsContext, JS_NewCFunctionData(jsContext, [](JSContext* /* jsContext */, JSValueConst /* thisValue */, int /* argc */, JSValueConst* /* argv */, int /* magic */, JSValueConst* /* functionData */) -> JSValue {
            return JS_UNDEFINED;
        }, 0, 0, 1, &functionData));
        ASSERT_FALSE(JS_IsException(callback.get()));
        JSValueConst callbackValue = callback.get();
        const ScopedJSValue result(jsContext, queueMicrotask(jsContext, JS_UNDEFINED, 1, &callbackValue));
        ASSERT_FALSE(JS_IsException(result.get()));
    }
    JS_RunGC(jsRuntime);
    EXPECT_FALSE(callbackFinalized);
    EXPECT_TRUE(JS_IsJobPending(jsRuntime));
    JS_FreeContext(jsContext);
    jsContext = nullptr;
    JS_FreeRuntime(jsRuntime);
    jsRuntime = nullptr;
    EXPECT_TRUE(callbackFinalized);
}

} // namespace
} // namespace amj::javascript
