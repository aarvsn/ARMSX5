#include "armsx5/system/kernel.h"

#include "armsx5/core/log.h"
#include "armsx5/cpu/cpu_state.h"
#include "armsx5/cpu/interpreter.h"
#include "armsx5/system/syscall.h"

#include <cstdio>
#include <cstring>

namespace armsx5::system {
namespace {
constexpr const char* TAG = "Kernel";
} // namespace

void StderrSink::write(const uint8_t* data, size_t len) {
    std::fwrite(data, 1, len, stderr);
}

void CaptureSink::write(const uint8_t* data, size_t len) {
    captured_.append(reinterpret_cast<const char*>(data), len);
}

Kernel::Kernel(memory::GuestMemory& mem) : mem_(mem), sink_(&default_sink()) {}

OutputSink& Kernel::default_sink() {
    static StderrSink s;
    return s;
}

Status Kernel::on_syscall(cpu::CpuState& state, bool& stop) {
    const uint64_t nr = state.gpr[cpu::reg::AX];
    const uint64_t a1 = state.gpr[cpu::reg::DI];
    const uint64_t a2 = state.gpr[cpu::reg::SI];
    const uint64_t a3 = state.gpr[cpu::reg::DX];

    auto set_result = [&](uint64_t ret) {
        state.gpr[cpu::reg::AX] = ret;
        state.flags.cf = false;
    };
    auto set_error = [&](uint64_t errno_val) {
        state.gpr[cpu::reg::AX] = errno_val;
        state.flags.cf = true;
        ++stats_.failed_calls;
    };

    switch (nr) {
    case fbsd::kExit: {
        last_exit_code_ = a1;
        stop = true;
        // Put the exit code in RAX: the run loop reports it via RAX because
        // FreeBSD exit() has no return path.
        state.gpr[cpu::reg::AX] = a1;
        state.flags.cf = false;
        ARMSX5_LOG_DEBUG(TAG, "guest exit(%llu)", static_cast<unsigned long long>(a1));
        return Status::Ok;
    }
    case fbsd::kWrite: {
        ++stats_.write_calls;
        if (a1 != 1 && a1 != 2) {
            set_error(fbsd::kEBADF);
            return Status::Ok;
        }
        if (a3 == 0) {
            set_result(0);
            return Status::Ok;
        }
        auto ptr = mem_.translate(a2, static_cast<size_t>(a3), memory::Prot::R);
        if (!ptr) {
            set_error(fbsd::kEINVAL);
            return Status::Ok;
        }
        if (sink_) sink_->write(static_cast<const uint8_t*>(*ptr), static_cast<size_t>(a3));
        set_result(a3);
        return Status::Ok;
    }
    case fbsd::kRead: {
        // Reads are served by the VFS in later phases; stdin is not wired.
        set_error(fbsd::kEBADF);
        return Status::Ok;
    }
    case fbsd::kMmap: {
        ++stats_.mmap_calls;
        const uint64_t hint = a1;
        const uint64_t len = a2;
        const uint64_t prot = a3;
        if (len == 0 || len > mem_.address_space_size()) {
            set_error(fbsd::kEINVAL);
            return Status::Ok;
        }
        memory::Prot p = memory::Prot::None;
        if (prot & fbsd::kProtRead) p = p | memory::Prot::R;
        if (prot & fbsd::kProtWrite) p = p | memory::Prot::W;
        if (prot & fbsd::kProtExec) p = p | memory::Prot::X;

        if (hint != 0 && hint % memory::GuestMemory::kPageSize == 0) {
            auto mapped = mem_.map(hint, static_cast<size_t>(len), p);
            if (!mapped) {
                set_error(fbsd::kENOMEM);
                return Status::Ok;
            }
            set_result(hint);
            return Status::Ok;
        }
        auto alloc = mem_.allocate(static_cast<size_t>(len), memory::GuestMemory::kPageSize, p);
        if (!alloc) {
            set_error(fbsd::kENOMEM);
            return Status::Ok;
        }
        set_result(*alloc);
        return Status::Ok;
    }
    case fbsd::kMunmap: {
        ++stats_.munmap_calls;
        auto r = mem_.unmap(a1);
        if (!r) {
            set_error(fbsd::kEINVAL);
            return Status::Ok;
        }
        set_result(0);
        return Status::Ok;
    }
    case fbsd::kMprotect: {
        ++stats_.mprotect_calls;
        memory::Prot p = memory::Prot::None;
        if (a3 & fbsd::kProtRead) p = p | memory::Prot::R;
        if (a3 & fbsd::kProtWrite) p = p | memory::Prot::W;
        if (a3 & fbsd::kProtExec) p = p | memory::Prot::X;
        auto r = mem_.protect(a1, static_cast<size_t>(a2), p);
        if (!r) {
            set_error(fbsd::kEINVAL);
            return Status::Ok;
        }
        set_result(0);
        return Status::Ok;
    }
    case fbsd::kSchedYield: {
        ++stats_.yield_calls;
        set_result(0); // single-threaded cooperative scheduling for now
        return Status::Ok;
    }
    default: {
        ++stats_.unsupported_calls;
        ARMSX5_LOG_DEBUG(TAG, "unsupported syscall %llu -> ENOSYS",
                         static_cast<unsigned long long>(nr));
        set_error(fbsd::kENOSYS);
        return Status::Ok;
    }
    }
}

} // namespace armsx5::system
