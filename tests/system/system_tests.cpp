// System-layer tests: kernel syscalls, VFS, HLE registry.
#include <gtest/gtest.h>

#include "armsx5/core/result.h"
#include "armsx5/cpu/cpu_state.h"
#include "armsx5/memory/guest_memory.h"
#include "armsx5/system/kernel.h"
#include "armsx5/system/module_registry.h"
#include "armsx5/system/syscall.h"
#include "armsx5/system/vfs.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

using namespace armsx5;
using namespace armsx5::system;

namespace {

// Prepares a CpuState for a syscall with args.
cpu::CpuState syscall_state(uint64_t nr, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    cpu::CpuState s;
    s.gpr[cpu::reg::AX] = nr;
    s.gpr[cpu::reg::DI] = a1;
    s.gpr[cpu::reg::SI] = a2;
    s.gpr[cpu::reg::DX] = a3;
    return s;
}

// Creates the host-dir fixture: /tmp/armsx5_vfs_test_root/hostfile.bin = "abcdef".
std::expected<HostDirectoryFS, Error> make_vfs_fixture() {
    ::mkdir("/tmp/armsx5_vfs_test_root", 0755);
    FILE* f = fopen("/tmp/armsx5_vfs_test_root/hostfile.bin", "wb");
    if (f) {
        fwrite("abcdef", 1, 6, f);
        fclose(f);
    }
    return HostDirectoryFS::create("/tmp/armsx5_vfs_test_root");
}

} // namespace

TEST(Kernel, ExitSetsStopAndCarriesCode) {
    memory::GuestMemory mem;
    system::Kernel k(mem);
    bool stop = false;
    auto s = syscall_state(fbsd::kExit, 42);
    ASSERT_EQ(k.on_syscall(s, stop), Status::Ok);
    EXPECT_TRUE(stop);
    EXPECT_EQ(k.last_exit_code(), 42u);
    EXPECT_TRUE(s.gpr[cpu::reg::AX] == 42u);
    EXPECT_FALSE(s.flags.cf);
}

TEST(Kernel, WriteCapturesData) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x10000, 0x1000, memory::Prot::R | memory::Prot::W).has_value());
    const char msg[] = "kernel-test";
    ASSERT_TRUE(mem.write(0x10000, msg, sizeof(msg) - 1).has_value());

    system::CaptureSink sink;
    system::Kernel k(mem);
    k.set_output_sink(&sink);
    bool stop = false;
    auto s = syscall_state(fbsd::kWrite, 1, 0x10000, sizeof(msg) - 1);
    ASSERT_EQ(k.on_syscall(s, stop), Status::Ok);
    EXPECT_FALSE(stop);
    EXPECT_EQ(s.gpr[cpu::reg::AX], sizeof(msg) - 1);
    EXPECT_FALSE(s.flags.cf);
    EXPECT_EQ(sink.captured(), "kernel-test");
    EXPECT_EQ(k.stats().write_calls, 1u);
}

TEST(Kernel, WriteBadFdReturnsEBADFWithCarry) {
    memory::GuestMemory mem;
    system::Kernel k(mem);
    bool stop = false;
    auto s = syscall_state(fbsd::kWrite, 7, 0, 1);
    ASSERT_EQ(k.on_syscall(s, stop), Status::Ok);
    EXPECT_EQ(s.gpr[cpu::reg::AX], fbsd::kEBADF);
    EXPECT_TRUE(s.flags.cf);
}

TEST(Kernel, MmapAllocatesAndMunmapFrees) {
    memory::GuestMemory mem;
    system::Kernel k(mem);
    bool stop = false;
    auto s = syscall_state(fbsd::kMmap, 0, 0x2000, fbsd::kProtRead | fbsd::kProtWrite);
    ASSERT_EQ(k.on_syscall(s, stop), Status::Ok);
    EXPECT_FALSE(s.flags.cf);
    const uint64_t addr = s.gpr[cpu::reg::AX];
    EXPECT_NE(addr, 0u);
    EXPECT_EQ(mem.mapped_bytes(), 0x2000u);

    auto s2 = syscall_state(fbsd::kMunmap, addr, 0, 0);
    ASSERT_EQ(k.on_syscall(s2, stop), Status::Ok);
    EXPECT_EQ(mem.mapped_bytes(), 0u);
}

TEST(Kernel, UnsupportedSyscallReturnsENOSYS) {
    memory::GuestMemory mem;
    system::Kernel k(mem);
    bool stop = false;
    auto s = syscall_state(9999); // not in the implemented set
    ASSERT_EQ(k.on_syscall(s, stop), Status::Ok);
    EXPECT_EQ(s.gpr[cpu::reg::AX], fbsd::kENOSYS);
    EXPECT_TRUE(s.flags.cf);
    EXPECT_EQ(k.stats().unsupported_calls, 1u);
}

TEST(Kernel, SchedYieldSucceeds) {
    memory::GuestMemory mem;
    system::Kernel k(mem);
    bool stop = false;
    auto s = syscall_state(fbsd::kSchedYield);
    ASSERT_EQ(k.on_syscall(s, stop), Status::Ok);
    EXPECT_EQ(s.gpr[cpu::reg::AX], 0u);
    EXPECT_FALSE(s.flags.cf);
}

// ---------- VFS ----------
TEST(Vfs, MemFsRoundTrip) {
    system::MemFS fs;
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    fs.put("test.bin", data);
    auto f = fs.open("test.bin");
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ((*f)->size(), 5u);
    uint8_t out[5] = {};
    auto r = (*f)->read_at(0, out, 5);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 5u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[4], 5);
}

TEST(Vfs, MemFsMissingFileNotFound) {
    system::MemFS fs;
    auto f = fs.open("nope.bin");
    ASSERT_FALSE(f.has_value());
    EXPECT_EQ(f.error().code, Status::NotFound);
}

TEST(Vfs, HostDirRejectsTraversal) {
    auto fsx = make_vfs_fixture();
    ASSERT_TRUE(fsx.has_value());
    HostDirectoryFS fs_ = std::move(*fsx);

    auto a = fs_.open("../outside.txt", false);
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().code, Status::InvalidArgument);

    auto b = fs_.open("/etc/passwd", false);
    ASSERT_FALSE(b.has_value());
    EXPECT_EQ(b.error().code, Status::InvalidArgument);

    auto c = fs_.open("sub/../../escape.txt", false);
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code, Status::InvalidArgument);
}

TEST(Vfs, HostDirOpensRealFile) {
    auto fsx = make_vfs_fixture();
    ASSERT_TRUE(fsx.has_value());
    HostDirectoryFS fs_ = std::move(*fsx);
    auto f = fs_.open("hostfile.bin", false);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ((*f)->size(), 6u);
    uint8_t out[6] = {};
    ASSERT_TRUE((*f)->read_at(0, out, 6).has_value());
    EXPECT_EQ(std::memcmp(out, "abcdef", 6), 0);
}

// ---------- HLE registry ----------
TEST(ModuleRegistry, RegistersAndQueries) {
    system::ModuleRegistry reg;
    reg.register_module("libSceGnmDriver", {"sceGnmDrawIndex", "sceGnmSetContextReg"}, 0);
    EXPECT_TRUE(reg.has_module("libSceGnmDriver"));
    EXPECT_FALSE(reg.has_module("libSceUnknown"));
    const auto* m = reg.module("libSceGnmDriver");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->exports.size(), 2u);
    EXPECT_EQ(m->implemented_count, 0u);
}

TEST(ModuleRegistry, UnimplementedCallsAreRecordedNotFaked) {
    system::ModuleRegistry reg;
    reg.register_module("libSceAudioOut", {"sceAudioOutOpen"}, 0);
    EXPECT_FALSE(reg.record_unimplemented("libSceAudioOut", "sceAudioOutOpen"));
    ASSERT_EQ(reg.unimplemented_calls().size(), 1u);
    EXPECT_EQ(reg.unimplemented_calls()[0], "libSceAudioOut::sceAudioOutOpen");
}
