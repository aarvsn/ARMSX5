// armsx5-runner: loads and executes a plain ELF64 x86-64 test executable
// through the ARMSX5 pipeline and prints a session report. Development
// diagnostic tool — plain ELF only (SELF containers are rejected).
#include "armsx5/core/log.h"
#include "armsx5/system/process.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: armsx5-runner <elf-file> [--steps N] [--log LEVEL]\n");
        return 2;
    }
    const std::string path = argv[1];
    armsx5::system::RunOptions opts;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--steps" && i + 1 < argc) opts.max_steps = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--log" && i + 1 < argc) {
            const std::string lvl = argv[++i];
            if (lvl == "trace") armsx5::log::set_level(armsx5::log::Level::Trace);
            else if (lvl == "debug") armsx5::log::set_level(armsx5::log::Level::Debug);
            else if (lvl == "warn") armsx5::log::set_level(armsx5::log::Level::Warn);
            else if (lvl == "error") armsx5::log::set_level(armsx5::log::Level::Error);
        }
    }

    armsx5::system::ProcessRuntime rt(opts);
    auto r = rt.run_file(path);
    if (!r.has_value()) {
        std::fprintf(stderr, "run failed: [%s] %s\n", armsx5::status_name(r.error().code),
                     r.error().message.c_str());
        return 3;
    }
    const auto& rr = *r;
    std::printf("=== ARMSX5 session report ===\n");
    std::printf("entry: 0x%llx  loads: %u  image: %llu bytes\n",
                static_cast<unsigned long long>(rr.metadata.entry), rr.metadata.load_count,
                static_cast<unsigned long long>(rr.metadata.total_load_memsz));
    std::printf("exited: %s  exit_code: %llu  steps: %llu  mapped: %llu KB\n",
                rr.exited ? "yes" : "no", static_cast<unsigned long long>(rr.exit_code),
                static_cast<unsigned long long>(rr.steps),
                static_cast<unsigned long long>(rr.mapped_bytes / 1024));
    if (!rr.detail.empty()) std::printf("detail: %s\n", rr.detail.c_str());
    std::printf("kernel: write=%llu mmap=%llu unsupported=%llu failed=%llu\n",
                static_cast<unsigned long long>(rr.kernel_stats.write_calls),
                static_cast<unsigned long long>(rr.kernel_stats.mmap_calls),
                static_cast<unsigned long long>(rr.kernel_stats.unsupported_calls),
                static_cast<unsigned long long>(rr.kernel_stats.failed_calls));
    if (!rr.stdout_text.empty()) std::printf("stdout: %s", rr.stdout_text.c_str());
    return rr.exited ? static_cast<int>(rr.exit_code & 0xFF) : 4;
}
