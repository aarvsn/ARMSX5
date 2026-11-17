#pragma once
// ELF64 executable parser and guest image loader.
//
// Scope (Phase 2): robust validation of plain ELF64 x86-64 executables and
// mapping of PT_LOAD segments into the guest address space. PS5 SELF-encoded
// executables are detected and reported Unsupported (see
// docs/research/self-loading.md for the blocking analysis). Parsing a valid
// ELF does NOT imply a commercial PS5 executable can run.
#include "armsx5/core/result.h"
#include "armsx5/memory/guest_memory.h"

#include <expected>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace armsx5::loader {

struct ExecutableMetadata {
    uint16_t type = 0;       // ET_EXEC / ET_DYN
    uint16_t machine = 0;    // EM_X86_64
    uint64_t entry = 0;
    uint32_t phnum = 0;
    uint32_t load_count = 0;
    uint64_t total_load_memsz = 0;
    bool has_gnu_stack = false;
    bool gnu_stack_executable = false;
    size_t file_size = 0;
    std::string to_json() const;
};

struct LoadedSegment {
    uint64_t vaddr;
    size_t memsz;
    uint32_t flags; // elf::kPfX | kPfW | kPfR
};

struct LoadedImage {
    uint64_t entry = 0;
    uint64_t image_base = 0;
    uint64_t image_size = 0;
    std::vector<LoadedSegment> segments;
};

class ElfLoader {
public:
    // Validates the header and program headers without mapping anything.
    // Rejects: wrong magic/class/endianness/version/machine, SELF containers,
    // out-of-bounds program headers, out-of-file PT_LOAD segments, and
    // PT_INTERP (external interpreter unsupported).
    static std::expected<ExecutableMetadata, Error> inspect(const uint8_t* data, size_t size);

    // File convenience wrapper: reads the file (64 MiB cap) and inspects it.
    static std::expected<ExecutableMetadata, Error> inspect_file(const std::string& path);

    // Validates then maps all PT_LOAD segments into the guest address space
    // and copies file contents. ET_DYN images are allocated at a deterministic
    // base; ET_EXEC images use their own vaddrs.
    static std::expected<LoadedImage, Error> load(memory::GuestMemory& mem,
                                                  const uint8_t* data, size_t size);
};

} // namespace armsx5::loader
