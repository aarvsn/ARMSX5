#include "armsx5/core/log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace armsx5::log {
namespace {

std::mutex g_mutex;
Level g_level = Level::Info;

const char* level_tag(Level lvl) {
    switch (lvl) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO";
    case Level::Warn:  return "WARN";
    case Level::Error: return "ERROR";
    }
    return "?????";
}

#if defined(__ANDROID__)
int android_priority(Level lvl) {
    switch (lvl) {
    case Level::Trace: return ANDROID_LOG_VERBOSE;
    case Level::Debug: return ANDROID_LOG_DEBUG;
    case Level::Info:  return ANDROID_LOG_INFO;
    case Level::Warn:  return ANDROID_LOG_WARN;
    case Level::Error: return ANDROID_LOG_ERROR;
    }
    return ANDROID_LOG_UNKNOWN;
}
#endif

} // namespace

void set_level(Level level) { g_level = level; }

Level level() { return g_level; }

void message(Level lvl, const char* tag, const char* fmt, ...) {
    if (lvl < g_level) return;
    char text[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "[ARMSX5 %s %s] %s\n", level_tag(lvl), tag, text);
#if defined(__ANDROID__)
    __android_log_print(android_priority(lvl), "ARMSX5", "%s: %s", tag, text);
#endif
}

} // namespace armsx5::log
