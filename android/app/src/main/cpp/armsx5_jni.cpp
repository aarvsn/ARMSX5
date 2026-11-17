// ARMSX5 JNI bridge: Android frontend <-> native runtime.
// Explicit registration avoids fragile symbol name lookups.
#include <android/log.h>
#include <jni.h>

#include "armsx5/core/log.h"
#include "armsx5/core/result.h"
#include "armsx5/cpu/interpreter.h"
#include "armsx5/gpu/vulkan/device_info.h"
#include "armsx5/input/controller.h"
#include "armsx5/loader/elf_loader.h"
#include "armsx5/memory/guest_memory.h"
#include "armsx5/system/kernel.h"
#include "armsx5/system/process.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

constexpr const char* kTag = "Armsx5JNI";

JavaVM* g_vm = nullptr;

// ---- long-lived native runtime objects ----
struct Runtime {
    std::unique_ptr<armsx5::system::ProcessRuntime> process;
    bool initialized = false;
};
Runtime g_runtime;

armsx5::input::MappingConfig g_mapping;
std::unique_ptr<armsx5::input::StateAggregator> g_aggregator;

std::string to_std(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out(c ? c : "");
    env->ReleaseStringUTFChars(s, c);
    return out;
}

jstring to_jni(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

// ---- native implementations ----

jboolean native_init(JNIEnv* env, jobject, jstring config_json) {
    const std::string cfg = to_std(env, config_json);
    // Minimal bootstrap: the frontend only passes a few scalar keys today.
    (void)cfg;
    armsx5::log::set_level(armsx5::log::Level::Info);
    g_runtime.process = std::make_unique<armsx5::system::ProcessRuntime>();
    g_aggregator = std::make_unique<armsx5::input::StateAggregator>(g_mapping);
    g_runtime.initialized = true;
    ARMSX5_LOG_INFO(kTag, "native runtime initialized");
    return JNI_TRUE;
}

jboolean native_shutdown(JNIEnv*, jobject) {
    g_runtime.process.reset();
    g_aggregator.reset();
    g_runtime.initialized = false;
    ARMSX5_LOG_INFO(kTag, "native runtime shut down");
    return JNI_TRUE;
}

jstring native_device_info_json(JNIEnv* env, jobject) {
    std::string out = "{";
    // CPU facts the NDK can observe directly.
    out += "\"cores\":" + std::to_string(std::thread::hardware_concurrency());
    out += ",\"page_size\":" + std::to_string(sysconf(_SC_PAGESIZE));
    out += ",\"guest_address_space_bytes\":4294967296";

    // Vulkan probe: real runtime introspection (may report an error state).
    auto vk = armsx5::gpu::vulkan::probe_runtime();
    if (vk.has_value()) {
        out += ",\"vulkan\":" + vk->to_json();
    } else {
        out += ",\"vulkan\":{\"error\":\"" + vk.error().message + "\"}";
    }
    out += "}";
    return to_jni(env, out);
}

jint native_validate_executable(JNIEnv* env, jobject, jstring path) {
    const std::string p = to_std(env, path);
    auto meta = armsx5::loader::ElfLoader::inspect_file(p);
    return meta.has_value() ? 0 : static_cast<jint>(meta.error().code);
}

jstring native_executable_info_json(JNIEnv* env, jobject, jstring path) {
    const std::string p = to_std(env, path);
    auto meta = armsx5::loader::ElfLoader::inspect_file(p);
    if (!meta.has_value()) {
        return to_jni(env, std::string("{\"error\":\"") +
                               armsx5::status_name(meta.error().code) + "\",\"message\":\"" +
                               meta.error().message + "\"}");
    }
    return to_jni(env, meta->to_json());
}

jstring native_run_test_executable(JNIEnv* env, jobject, jstring path, jlong max_steps) {
    const std::string p = to_std(env, path);
    armsx5::system::RunOptions opts;
    opts.max_steps = static_cast<uint64_t>(max_steps > 0 ? max_steps : 100000000ull);
    auto r = g_runtime.process->run_file(p);
    if (!r.has_value()) {
        return to_jni(env, std::string("{\"status\":\"") +
                               armsx5::status_name(r.error().code) + "\",\"message\":\"" +
                               r.error().message + "\"}");
    }
    const auto& rr = *r;
    std::string out = "{";
    out += "\"status\":\"" + std::string(rr.exited ? "exited" : "stopped") + "\"";
    out += ",\"exit_code\":" + std::to_string(rr.exit_code);
    out += ",\"steps\":" + std::to_string(rr.steps);
    out += ",\"mapped_bytes\":" + std::to_string(rr.mapped_bytes);
    out += ",\"detail\":\"" + rr.detail + "\"";
    out += ",\"stdout\":\"" + rr.stdout_text + "\"";
    out += ",\"metadata\":" + rr.metadata.to_json();
    out += "}";
    return to_jni(env, out);
}

void native_on_controller_button(JNIEnv*, jobject, jint keycode, jboolean pressed) {
    if (g_aggregator) g_aggregator->on_button(static_cast<uint32_t>(keycode), pressed == JNI_TRUE);
}

void native_on_controller_axis(JNIEnv*, jobject, jint axis, jfloat value) {
    if (g_aggregator) g_aggregator->on_axis(static_cast<uint32_t>(axis), value);
}

jstring native_controller_state_json(JNIEnv* env, jobject) {
    if (!g_aggregator) return to_jni(env, std::string("{\"connected\":false}"));
    const auto& st = g_aggregator->state();
    std::string out = "{\"connected\":" + std::string(st.connected ? "true" : "false");
    out += ",\"buttons\":" + std::to_string(st.buttons);
    out += ",\"axes\":[";
    for (size_t i = 0; i < sizeof(st.axes) / sizeof(st.axes[0]); ++i) {
        if (i) out += ",";
        char buf[32];
        snprintf(buf, sizeof(buf), "%.4f", st.axes[i]);
        out += buf;
    }
    out += "]}";
    return to_jni(env, out);
}

void native_set_log_level(JNIEnv*, jobject, jint level) {
    if (level < 0 || level > 4) level = 2;
    armsx5::log::set_level(static_cast<armsx5::log::Level>(level));
}

// ---- registration ----

const JNINativeMethod kMethods[] = {
    {"nativeInit", "(Ljava/lang/String;)Z",
     reinterpret_cast<void*>(native_init)},
    {"nativeShutdown", "()Z", reinterpret_cast<void*>(native_shutdown)},
    {"nativeDeviceInfoJson", "()Ljava/lang/String;",
     reinterpret_cast<void*>(native_device_info_json)},
    {"nativeValidateExecutable", "(Ljava/lang/String;)I",
     reinterpret_cast<void*>(native_validate_executable)},
    {"nativeExecutableInfoJson", "(Ljava/lang/String;)Ljava/lang/String;",
     reinterpret_cast<void*>(native_executable_info_json)},
    {"nativeRunTestExecutable", "(Ljava/lang/String;J)Ljava/lang/String;",
     reinterpret_cast<void*>(native_run_test_executable)},
    {"nativeOnControllerButton", "(IZ)V",
     reinterpret_cast<void*>(native_on_controller_button)},
    {"nativeOnControllerAxis", "(IF)V",
     reinterpret_cast<void*>(native_on_controller_axis)},
    {"nativeControllerStateJson", "()Ljava/lang/String;",
     reinterpret_cast<void*>(native_controller_state_json)},
    {"nativeSetLogLevel", "(I)V", reinterpret_cast<void*>(native_set_log_level)},
};

} // namespace

// Must have external linkage: the Android runtime resolves JNI_OnLoad on
// library load and uses it to bind the explicit registration table.
jint JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass cls = env->FindClass("com/armsx5/data/Armsx5Native");
    if (cls == nullptr) return JNI_ERR;
    if (env->RegisterNatives(cls, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) != JNI_OK) {
        return JNI_ERR;
    }
    env->DeleteLocalRef(cls);
    return JNI_VERSION_1_6;
}
