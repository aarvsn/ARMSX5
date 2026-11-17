// Core + guest-memory unit tests.
#include <gtest/gtest.h>

#include "armsx5/core/config.h"
#include "armsx5/core/result.h"
#include "armsx5/memory/guest_memory.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace armsx5;

// ---------- config ----------
TEST(Config, TypedGettersReturnDefaults) {
    Config c;
    EXPECT_EQ(c.get_string("missing", "fallback"), "fallback");
    EXPECT_EQ(c.get_int("missing", 7), 7);
    EXPECT_EQ(c.get_bool("missing", true), true);
}

TEST(Config, SetGetRoundTrip) {
    Config c;
    c.set_string("cpu.backend", "interpreter");
    c.set_int("gpu.scale", 2);
    c.set_bool("jit.enabled", false);
    EXPECT_EQ(c.get_string("cpu.backend", ""), "interpreter");
    EXPECT_EQ(c.get_int("gpu.scale", 0), 2);
    EXPECT_EQ(c.get_bool("jit.enabled", true), false);
    EXPECT_EQ(c.size(), 3u);
    EXPECT_TRUE(c.contains("jit.enabled"));
}

TEST(Config, FileSaveLoadRoundTrip) {
    const std::string path = "/tmp/armsx5_test_config.cfg";
    {
        Config c;
        c.set_int("core.log_level", 3);
        c.set_bool("perf.overlay", true);
        ASSERT_TRUE(c.save_file(path));
    }
    Config c2;
    ASSERT_TRUE(c2.load_file(path));
    EXPECT_EQ(c2.get_int("core.log_level", 0), 3);
    EXPECT_EQ(c2.get_bool("perf.overlay", false), true);
    std::remove(path.c_str());
}

TEST(Config, LoadMissingFileFails) {
    Config c;
    EXPECT_FALSE(c.load_file("/tmp/definitely_missing_armsx5_file.cfg"));
}

TEST(Config, BadIntFallsBack) {
    Config c;
    c.set_string("x", "not_a_number");
    EXPECT_EQ(c.get_int("x", 5), 5);
}

// ---------- guest memory ----------
TEST(GuestMemory, MapAndReadBack) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x10000, 0x1000, memory::Prot::R | memory::Prot::W).has_value());
    const uint64_t v = 0x1234567890abcdefULL;
    ASSERT_TRUE(mem.write(0x10100, &v, 8).has_value());
    uint64_t out = 0;
    ASSERT_TRUE(mem.read(0x10100, &out, 8).has_value());
    EXPECT_EQ(out, v);
    EXPECT_EQ(mem.mapped_bytes(), 0x1000u);
}

TEST(GuestMemory, RejectsUnalignedBase) {
    memory::GuestMemory mem;
    auto r = mem.map(0x10001, 0x1000, memory::Prot::R);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Status::InvalidArgument);
}

TEST(GuestMemory, RejectsOverlap) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x20000, 0x2000, memory::Prot::R).has_value());
    auto r = mem.map(0x21000, 0x1000, memory::Prot::R);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Status::InvalidArgument);
}

TEST(GuestMemory, UnmappedReadIsGuestFault) {
    memory::GuestMemory mem;
    uint64_t v = 0;
    auto r = mem.read(0xdead0000, &v, 8);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Status::GuestFault);
}

TEST(GuestMemory, BoundsCheckAcrossBoundary) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x30000, 0x1000, memory::Prot::R | memory::Prot::W).has_value());
    auto t = mem.translate(0x3FF8, 16, memory::Prot::R);
    ASSERT_FALSE(t.has_value());
    EXPECT_EQ(t.error().code, Status::GuestFault);
}

TEST(GuestMemory, PermissionEnforced) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x40000, 0x1000, memory::Prot::R).has_value());
    uint64_t v = 1;
    auto w = mem.write(0x40000, &v, 8);
    ASSERT_FALSE(w.has_value());
    EXPECT_EQ(w.error().code, Status::GuestFault);
    auto r = mem.read(0x40000, &v, 8);
    ASSERT_TRUE(r.has_value());
}

TEST(GuestMemory, ProtectUpdatesPermissions) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x50000, 0x1000, memory::Prot::R).has_value());
    ASSERT_TRUE(mem.protect(0x50000, 0x1000, memory::Prot::R | memory::Prot::W).has_value());
    uint64_t v = 99;
    ASSERT_TRUE(mem.write(0x50000, &v, 8).has_value());
    EXPECT_EQ(mem.mapping_count(), 1u);
}

TEST(GuestMemory, ProtectPartialRangeRejected) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x60000, 0x2000, memory::Prot::R).has_value());
    auto r = mem.protect(0x60000, 0x1000, memory::Prot::R | memory::Prot::W);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Status::InvalidArgument);
}

TEST(GuestMemory, UnmapRemovesAndClearsExec) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x70000, 0x1000, memory::Prot::R | memory::Prot::X).has_value());
    mem.mark_exec(0x70000, 0x1000);
    EXPECT_TRUE(mem.is_exec_page(0x70800));
    ASSERT_TRUE(mem.unmap(0x70000).has_value());
    EXPECT_FALSE(mem.is_exec_page(0x70800));
    EXPECT_EQ(mem.mapping_count(), 0u);
}

TEST(GuestMemory, BumpAllocatorAlignedAndSequential) {
    memory::GuestMemory mem;
    auto a = mem.allocate(0x1000, 0x1000, memory::Prot::R | memory::Prot::W);
    ASSERT_TRUE(a.has_value());
    auto b = mem.allocate(0x1000, 0x1000, memory::Prot::R | memory::Prot::W);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*a % 0x1000, 0u);
    EXPECT_EQ(*b % 0x1000, 0u);
    EXPECT_EQ(*b, *a + 0x1000);
}

TEST(GuestMemory, ExecPageTracking) {
    memory::GuestMemory mem;
    ASSERT_TRUE(mem.map(0x80000, 0x2000, memory::Prot::R | memory::Prot::X).has_value());
    mem.mark_exec(0x80000, 0x1000); // only the first page
    EXPECT_TRUE(mem.is_exec_page(0x80000));
    EXPECT_TRUE(mem.is_exec_page(0x80FFF));
    EXPECT_FALSE(mem.is_exec_page(0x81000));
}

TEST(GuestMemory, AddressSpaceLimitEnforced) {
    memory::GuestMemory mem(0x100000); // 1 MiB address space
    auto r = mem.map(0x100000, 0x1000, memory::Prot::R);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Status::InvalidArgument);
}
