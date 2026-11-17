#pragma once
// ARMSX5 core logging. Portable core must not depend on Android headers directly;
// log.cpp isolates the platform sink. printf-style, thread-safe, level filtered.
#include <cstdint>

namespace armsx5::log {

enum class Level : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

void set_level(Level level);
Level level();

void message(Level lvl, const char* tag, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

} // namespace armsx5::log

#define ARMSX5_LOG_TRACE(tag, ...) ::armsx5::log::message(::armsx5::log::Level::Trace, tag, __VA_ARGS__)
#define ARMSX5_LOG_DEBUG(tag, ...) ::armsx5::log::message(::armsx5::log::Level::Debug, tag, __VA_ARGS__)
#define ARMSX5_LOG_INFO(tag, ...)  ::armsx5::log::message(::armsx5::log::Level::Info, tag, __VA_ARGS__)
#define ARMSX5_LOG_WARN(tag, ...)  ::armsx5::log::message(::armsx5::log::Level::Warn, tag, __VA_ARGS__)
#define ARMSX5_LOG_ERROR(tag, ...) ::armsx5::log::message(::armsx5::log::Level::Error, tag, __VA_ARGS__)
