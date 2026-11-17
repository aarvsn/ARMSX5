#pragma once
// ARMSX5 status codes and error values shared by every subsystem.
#include <string>
#include <utility>

namespace armsx5 {

enum class Status : int {
    Ok = 0,
    InvalidArgument = 1,
    Unsupported = 2,
    NotFound = 3,
    ParseError = 4,
    MemoryError = 5,
    IoError = 6,
    JitError = 7,
    VulkanError = 8,
    KernelError = 9,
    GuestFault = 10,
    InternalError = 11,
};

inline const char* status_name(Status code) {
    switch (code) {
    case Status::Ok: return "Ok";
    case Status::InvalidArgument: return "InvalidArgument";
    case Status::Unsupported: return "Unsupported";
    case Status::NotFound: return "NotFound";
    case Status::ParseError: return "ParseError";
    case Status::MemoryError: return "MemoryError";
    case Status::IoError: return "IoError";
    case Status::JitError: return "JitError";
    case Status::VulkanError: return "VulkanError";
    case Status::KernelError: return "KernelError";
    case Status::GuestFault: return "GuestFault";
    case Status::InternalError: return "InternalError";
    }
    return "Unknown";
}

struct Error {
    Status code = Status::InternalError;
    std::string message;

    Error() = default;
    Error(Status c, std::string m) : code(c), message(std::move(m)) {}
};

} // namespace armsx5
