#pragma once
// Self-contained ELF64 structures and constants (System V AMD64 ABI).
// Kept local to ARMSX5 so the loader has no external dependency.
#include <cstdint>
#include <cstddef>

namespace armsx5::loader::elf {

inline constexpr uint8_t kMagic0 = 0x7f;
inline constexpr char kMagic1 = 'E', kMagic2 = 'L', kMagic3 = 'F';
inline constexpr uint8_t kClass64 = 2;
inline constexpr uint8_t kDataLittleEndian = 1;
inline constexpr uint8_t kVersionCurrent = 1;
inline constexpr uint16_t kTypeExec = 2;
inline constexpr uint16_t kTypeDyn = 3;
inline constexpr uint16_t kMachineX8664 = 62;

inline constexpr uint32_t kPtLoad = 1;
inline constexpr uint32_t kPtDynamic = 2;
inline constexpr uint32_t kPtInterp = 3;
inline constexpr uint32_t kPtPhdr = 6;
inline constexpr uint32_t kPtGnuStack = 0x6474e551;
inline constexpr uint32_t kPtGnuRelro = 0x6474e552;

inline constexpr uint32_t kPfX = 1, kPfW = 2, kPfR = 4;

struct Ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

static_assert(sizeof(Ehdr) == 64, "ELF64 ehdr must be 64 bytes");
static_assert(sizeof(Phdr) == 56, "ELF64 phdr must be 56 bytes");

// PS5 SELF container magic ("SCE\1" at file offset 0). Not parsed yet; the
// loader reports Unsupported with a pointer to the research notes.
inline constexpr uint8_t kSelfMagic[4] = {'S', 'C', 'E', 0x01};

} // namespace armsx5::loader::elf
