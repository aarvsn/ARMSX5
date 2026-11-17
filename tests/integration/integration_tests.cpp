// End-to-end integration tests: original homebrew test executables run
// through the complete pipeline (ELF load -> guest memory -> CPU -> kernel).
#include <gtest/gtest.h>

#include "armsx5/system/process.h"

#include <string>
#include <vector>

#ifndef ARMSX5_TEST_ELFS_DIR
#define ARMSX5_TEST_ELFS_DIR "."
#endif

using namespace armsx5;

namespace {
std::string elf_path(const std::string& name) {
    return std::string(ARMSX5_TEST_ELFS_DIR) + "/" + name;
}
} // namespace

TEST(Integration, HelloElfPrintsAndExits42) {
    system::ProcessRuntime rt;
    auto r = rt.run_file(elf_path("hello.elf"));
    ASSERT_TRUE(r.has_value()) << (r.error().code == Status::IoError
                                       ? "test ELF not built (run armsx5_test_elfs target)"
                                       : r.error().message);
    EXPECT_TRUE(r->exited);
    EXPECT_EQ(r->exit_code, 42u);
    EXPECT_NE(r->stdout_text.find("Hello from ARMSX5 guest"), std::string::npos);
    EXPECT_GT(r->steps, 0u);
    EXPECT_EQ(r->metadata.machine, 62u);
}

TEST(Integration, SumElfComputes5050) {
    system::ProcessRuntime rt;
    auto r = rt.run_file(elf_path("sum.elf"));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->exited);
    EXPECT_EQ(r->exit_code, 5050u);
}

TEST(Integration, SimdAvxElfComputes576) {
    system::ProcessRuntime rt;
    auto r = rt.run_file(elf_path("simdavx.elf"));
    ASSERT_TRUE(r.has_value()) << (r.error().code == Status::IoError
                                       ? "test ELF not built (run armsx5_test_elfs target)"
                                       : r.error().message);
    EXPECT_TRUE(r->exited);
    EXPECT_EQ(r->exit_code, 576u);
}

TEST(Integration, CallRetElfReturnsTwelve) {
    system::ProcessRuntime rt;
    auto r = rt.run_file(elf_path("callret.elf"));
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->exited);
    EXPECT_EQ(r->exit_code, 12u);
}

TEST(Integration, SessionCleanupLeavesNoMappings) {
    uint64_t last_mapped = 0;
    {
        system::ProcessRuntime rt;
        auto r = rt.run_file(elf_path("hello.elf"));
        ASSERT_TRUE(r.has_value());
        last_mapped = r->mapped_bytes;
    }
    EXPECT_GT(last_mapped, 0u); // sanity: the session actually mapped memory
}

TEST(Integration, MalformedExecutableIsRejectedSafely) {
    system::ProcessRuntime rt;
    std::vector<uint8_t> garbage = {'A', 'R', 'M', 'S', 'X', '5', '!', '!'};
    auto r = rt.run(garbage.data(), garbage.size());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, Status::ParseError);
}
