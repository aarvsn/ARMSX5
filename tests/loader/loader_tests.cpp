// ELF64 loader tests: valid images and the malformed-input battery.
#include <gtest/gtest.h>

#include "armsx5/loader/elf.h"
#include "armsx5/loader/elf_loader.h"
#include "armsx5/memory/guest_memory.h"

#include <cstring>
#include <vector>

using namespace armsx5;
using armsx5::loader::ElfLoader;
using armsx5::loader::ExecutableMetadata;

namespace {

// Builds a minimal but structurally-valid ELF64 executable.
std::vector<uint8_t> make_elf(uint64_t vaddr, uint64_t memsz, uint64_t filesz, uint32_t flags,
                              uint64_t entry, uint16_t e_type = 2) {
    using namespace armsx5::loader::elf;
    const size_t phnum = 1;
    std::vector<uint8_t> out(sizeof(Ehdr) + sizeof(Phdr) + filesz, 0);
    Ehdr h{};
    h.e_ident[0] = kMagic0;
    h.e_ident[1] = kMagic1;
    h.e_ident[2] = kMagic2;
    h.e_ident[3] = kMagic3;
    h.e_ident[4] = kClass64;
    h.e_ident[5] = kDataLittleEndian;
    h.e_ident[6] = kVersionCurrent;
    h.e_type = e_type;
    h.e_machine = kMachineX8664;
    h.e_version = kVersionCurrent;
    h.e_entry = entry;
    h.e_phoff = sizeof(Ehdr);
    h.e_phentsize = sizeof(Phdr);
    h.e_phnum = phnum;
    Phdr p{};
    p.p_type = kPtLoad;
    p.p_flags = flags;
    p.p_offset = sizeof(Ehdr) + sizeof(Phdr);
    p.p_vaddr = vaddr;
    p.p_filesz = filesz;
    p.p_memsz = memsz;
    p.p_align = 0x1000;
    std::memcpy(out.data(), &h, sizeof(h));
    std::memcpy(out.data() + sizeof(h), &p, sizeof(p));
    // Fill file contents with a recognizable pattern.
    for (size_t i = 0; i < filesz; ++i) out[sizeof(Ehdr) + sizeof(Phdr) + i] = 0xA5;
    return out;
}

} // namespace

TEST(ElfLoader, AcceptsMinimalValidExecutable) {
    auto elf = make_elf(0x400000, 0x2000, 0x1000, 5 /*R|X*/, 0x400000);
    auto meta = ElfLoader::inspect(elf.data(), elf.size());
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->type, 2);
    EXPECT_EQ(meta->machine, 62);
    EXPECT_EQ(meta->entry, 0x400000u);
    EXPECT_EQ(meta->load_count, 1u);
    EXPECT_EQ(meta->total_load_memsz, 0x2000u);
}

TEST(ElfLoader, LoadMapsSegmentsAndEntryInsideImage) {
    auto elf = make_elf(0x400000, 0x2000, 0x1000, 5, 0x400000 + 0x100);
    memory::GuestMemory mem;
    auto img = ElfLoader::load(mem, elf.data(), elf.size());
    ASSERT_TRUE(img.has_value());
    EXPECT_EQ(img->entry, 0x400100u);
    ASSERT_EQ(img->segments.size(), 1u);
    EXPECT_EQ(img->segments[0].vaddr, 0x400000u);
    // Content copied byte-exact.
    uint8_t probe = 0;
    ASSERT_TRUE(mem.read(0x400000 + 5, &probe, 1).has_value());
    EXPECT_EQ(probe, 0xA5);
    // Image pages marked executable.
    EXPECT_TRUE(mem.is_exec_page(0x400000));
}

TEST(ElfLoader, DynImageGetsAllocatedBase) {
    auto elf = make_elf(0x0, 0x2000, 0x1000, 5, 0x40, 3 /*ET_DYN*/);
    memory::GuestMemory mem;
    auto img = ElfLoader::load(mem, elf.data(), elf.size());
    ASSERT_TRUE(img.has_value());
    EXPECT_GT(img->image_base, 0u);
    EXPECT_EQ(img->entry, img->image_base + 0x40);
}

TEST(ElfLoader, RejectsBadMagic) {
    auto elf = make_elf(0x400000, 0x1000, 0x1000, 5, 0x400000);
    elf[0] = 'Z';
    auto meta = ElfLoader::inspect(elf.data(), elf.size());
    ASSERT_FALSE(meta.has_value());
    EXPECT_EQ(meta.error().code, Status::ParseError);
}

TEST(ElfLoader, RejectsTruncatedHeader) {
    auto elf = make_elf(0x400000, 0x1000, 0x1000, 5, 0x400000);
    auto meta = ElfLoader::inspect(elf.data(), sizeof(armsx5::loader::elf::Ehdr) - 1);
    ASSERT_FALSE(meta.has_value());
    EXPECT_EQ(meta.error().code, Status::ParseError);
}

TEST(ElfLoader, RejectsWrongMachine) {
    auto elf = make_elf(0x400000, 0x1000, 0x1000, 5, 0x400000);
    auto* h = reinterpret_cast<armsx5::loader::elf::Ehdr*>(elf.data());
    h->e_machine = 40; // EM_ARM
    auto meta = ElfLoader::inspect(elf.data(), elf.size());
    ASSERT_FALSE(meta.has_value());
    EXPECT_EQ(meta.error().code, Status::Unsupported);
}

TEST(ElfLoader, RejectsWrongClass) {
    auto elf = make_elf(0x400000, 0x1000, 0x1000, 5, 0x400000);
    elf[4] = 1; // ELFCLASS32
    auto meta = ElfLoader::inspect(elf.data(), elf.size());
    ASSERT_FALSE(meta.has_value());
    EXPECT_EQ(meta.error().code, Status::Unsupported);
}

TEST(ElfLoader, RejectsSegmentDataBeyondFile) {
    auto elf = make_elf(0x400000, 0x4000, 0x9000, 5, 0x400000); // filesz exceeds file
    auto meta = ElfLoader::inspect(elf.data(), elf.size());
    ASSERT_FALSE(meta.has_value());
    EXPECT_EQ(meta.error().code, Status::ParseError);
}

TEST(ElfLoader, RejectsFileszGreaterThanMemsz) {
    auto elf = make_elf(0x400000, 0x1000, 0x2000, 5, 0x400000);
    auto meta = ElfLoader::inspect(elf.data(), elf.size());
    ASSERT_FALSE(meta.has_value());
    EXPECT_EQ(meta.error().code, Status::ParseError);
}

TEST(ElfLoader, RejectsPhdrTableBeyondFile) {
    auto elf = make_elf(0x400000, 0x1000, 0x1000, 5, 0x400000);
    auto* h = reinterpret_cast<armsx5::loader::elf::Ehdr*>(elf.data());
    h->e_phoff = elf.size() + 1; // out of bounds
    auto meta = ElfLoader::inspect(elf.data(), elf.size());
    ASSERT_FALSE(meta.has_value());
    EXPECT_EQ(meta.error().code, Status::ParseError);
}

TEST(ElfLoader, RejectsEmptyImage) {
    auto meta = ElfLoader::inspect(nullptr, 0);
    ASSERT_FALSE(meta.has_value());
    EXPECT_EQ(meta.error().code, Status::InvalidArgument);
}

TEST(ElfLoader, RejectsNonExecElfType) {
    auto elf = make_elf(0x400000, 0x1000, 0x1000, 5, 0x400000, 1 /*ET_REL*/);
    auto meta = ElfLoader::inspect(elf.data(), elf.size());
    ASSERT_FALSE(meta.has_value());
    EXPECT_EQ(meta.error().code, Status::Unsupported);
}

TEST(ElfLoader, DetectsSelfContainer) {
    auto elf = make_elf(0x400000, 0x1000, 0x1000, 5, 0x400000);
    elf[0] = 'S';
    elf[1] = 'C';
    elf[2] = 'E';
    elf[3] = 0x01;
    auto meta = ElfLoader::inspect(elf.data(), elf.size());
    ASSERT_FALSE(meta.has_value());
    EXPECT_EQ(meta.error().code, Status::Unsupported);
    // The message must point at the research notes, not claim support.
    EXPECT_NE(meta.error().message.find("self-loading"), std::string::npos);
}

TEST(ElfLoader, RejectsEntryOutsideImage) {
    auto elf = make_elf(0x400000, 0x1000, 0x1000, 5, 0x500000); // entry not mapped
    memory::GuestMemory mem;
    auto img = ElfLoader::load(mem, elf.data(), elf.size());
    ASSERT_FALSE(img.has_value());
    EXPECT_EQ(img.error().code, Status::ParseError);
}

TEST(ElfLoader, MetadataJsonIsWellFormed) {
    auto elf = make_elf(0x400000, 0x2000, 0x1000, 5, 0x400000);
    auto meta = ElfLoader::inspect(elf.data(), elf.size());
    ASSERT_TRUE(meta.has_value());
    const std::string json = meta->to_json();
    EXPECT_NE(json.find("\"machine\":62"), std::string::npos);
    EXPECT_NE(json.find("\"entry\":4194304"), std::string::npos); // 0x400000
}
