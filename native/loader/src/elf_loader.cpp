#include "armsx5/loader/elf_loader.h"

#include "armsx5/core/log.h"
#include "armsx5/loader/elf.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace armsx5::loader {
namespace {
constexpr const char* TAG = "ElfLoader";

using elf::Ehdr;
using elf::Phdr;

memory::Prot flags_to_prot(uint32_t flags) {
    memory::Prot p = memory::Prot::None;
    if (flags & elf::kPfR) p = p | memory::Prot::R;
    if (flags & elf::kPfW) p = p | memory::Prot::W;
    if (flags & elf::kPfX) p = p | memory::Prot::X;
    return p;
}

std::expected<Ehdr, Error> parse_header(const uint8_t* data, size_t size) {
    if (size < sizeof(Ehdr))
        return std::unexpected(Error{Status::ParseError, "file smaller than ELF64 header"});

    Ehdr h{};
    std::memcpy(&h, data, sizeof(h));

    if (data[0] == elf::kSelfMagic[0] && data[1] == elf::kSelfMagic[1] &&
        data[2] == elf::kSelfMagic[2] && data[3] == elf::kSelfMagic[3]) {
        return std::unexpected(Error{
            Status::Unsupported,
            "SELF container detected: PS5 encrypted executables are not supported yet "
            "(docs/research/self-loading.md); ARMSX5 only loads plain ELF64 test executables"});
    }
    if (h.e_ident[0] != elf::kMagic0 || h.e_ident[1] != elf::kMagic1 ||
        h.e_ident[2] != elf::kMagic2 || h.e_ident[3] != elf::kMagic3) {
        return std::unexpected(Error{Status::ParseError, "bad ELF magic"});
    }
    if (h.e_ident[4] != elf::kClass64)
        return std::unexpected(Error{Status::Unsupported, "only ELF64 (class 2) is supported"});
    if (h.e_ident[5] != elf::kDataLittleEndian)
        return std::unexpected(Error{Status::Unsupported, "only little-endian executables are supported"});
    if (h.e_ident[6] != elf::kVersionCurrent)
        return std::unexpected(Error{Status::ParseError, "unknown ELF version"});
    if (h.e_machine != elf::kMachineX8664)
        return std::unexpected(Error{Status::Unsupported, "only EM_X86_64 executables are supported"});
    if (h.e_type != elf::kTypeExec && h.e_type != elf::kTypeDyn)
        return std::unexpected(Error{Status::Unsupported, "unsupported ELF type (relocatable/core)"});
    if (h.e_phentsize != sizeof(Phdr))
        return std::unexpected(Error{Status::ParseError, "unexpected e_phentsize"});
    if (h.e_phnum == 0)
        return std::unexpected(Error{Status::ParseError, "no program headers"});
    if (h.e_phoff == 0 || h.e_phoff + static_cast<uint64_t>(h.e_phnum) * sizeof(Phdr) > size)
        return std::unexpected(Error{Status::ParseError, "program header table out of file bounds"});
    return h;
}

std::expected<std::vector<Phdr>, Error> parse_phdrs(const Ehdr& h, const uint8_t* data,
                                                    size_t size) {
    std::vector<Phdr> phdrs;
    phdrs.reserve(h.e_phnum);
    for (uint32_t i = 0; i < h.e_phnum; ++i) {
        const size_t off = static_cast<size_t>(h.e_phoff) + i * sizeof(Phdr);
        if (off + sizeof(Phdr) > size)
            return std::unexpected(Error{Status::ParseError, "program header out of file bounds"});
        Phdr p{};
        std::memcpy(&p, data + off, sizeof(p));
        phdrs.push_back(p);
    }
    return phdrs;
}

} // namespace

std::string ExecutableMetadata::to_json() const {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"type\":%u,\"machine\":%u,\"entry\":%llu,\"phnum\":%u,\"loads\":%u,"
             "\"load_memsz\":%llu,\"file_size\":%zu,\"gnu_stack_exec\":%s}",
             type, machine, static_cast<unsigned long long>(entry), phnum, load_count,
             static_cast<unsigned long long>(total_load_memsz), file_size,
             gnu_stack_executable ? "true" : "false");
    return buf;
}

std::expected<ExecutableMetadata, Error> ElfLoader::inspect_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::unexpected(Error{Status::IoError, "cannot open " + path});
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > (64 << 20)) {
        fclose(f);
        return std::unexpected(Error{Status::IoError, "invalid file size: " + path});
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (size > 0 && fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        fclose(f);
        return std::unexpected(Error{Status::IoError, "short read: " + path});
    }
    fclose(f);
    return inspect(buf.data(), buf.size());
}

std::expected<ExecutableMetadata, Error> ElfLoader::inspect(const uint8_t* data, size_t size) {
    if (!data || size == 0)
        return std::unexpected(Error{Status::InvalidArgument, "empty executable image"});

    auto h = parse_header(data, size);
    if (!h) return std::unexpected(h.error());
    auto phdrs = parse_phdrs(*h, data, size);
    if (!phdrs) return std::unexpected(phdrs.error());

    ExecutableMetadata meta;
    meta.type = h->e_type;
    meta.machine = h->e_machine;
    meta.entry = h->e_entry;
    meta.phnum = h->e_phnum;
    meta.file_size = size;
    meta.has_gnu_stack = false;

    for (const Phdr& p : *phdrs) {
        if (p.p_type == elf::kPtLoad) {
            if (p.p_filesz > p.p_memsz)
                return std::unexpected(Error{Status::ParseError, "PT_LOAD filesz exceeds memsz"});
            if (p.p_filesz > 0 && p.p_offset + p.p_filesz > size)
                return std::unexpected(
                    Error{Status::ParseError, "PT_LOAD segment data out of file bounds"});
            meta.load_count++;
            meta.total_load_memsz += p.p_memsz;
        } else if (p.p_type == elf::kPtInterp) {
            return std::unexpected(Error{Status::Unsupported,
                                         "PT_INTERP: external program interpreters are not "
                                         "supported (no PS5 dynamic linker yet)"});
        } else if (p.p_type == elf::kPtGnuStack) {
            meta.has_gnu_stack = true;
            meta.gnu_stack_executable = (p.p_flags & elf::kPfX) != 0;
        }
    }
    if (meta.load_count == 0)
        return std::unexpected(Error{Status::ParseError, "no PT_LOAD segments"});
    return meta;
}

std::expected<LoadedImage, Error> ElfLoader::load(memory::GuestMemory& mem,
                                                  const uint8_t* data, size_t size) {
    auto meta = inspect(data, size);
    if (!meta) return std::unexpected(meta.error());

    auto h = parse_header(data, size);
    if (!h) return std::unexpected(h.error());
    auto phdrs = parse_phdrs(*h, data, size);
    if (!phdrs) return std::unexpected(phdrs.error());

    // Collect PT_LOADs and reject overlaps before mapping anything.
    std::vector<Phdr> loads;
    for (const Phdr& p : *phdrs)
        if (p.p_type == elf::kPtLoad) loads.push_back(p);
    std::sort(loads.begin(), loads.end(),
              [](const Phdr& a, const Phdr& b) { return a.p_vaddr < b.p_vaddr; });
    for (size_t i = 1; i < loads.size(); ++i) {
        const uint64_t prev_end = loads[i - 1].p_vaddr + loads[i - 1].p_memsz;
        if (prev_end > loads[i].p_vaddr)
            return std::unexpected(Error{Status::ParseError, "overlapping PT_LOAD segments"});
    }

    LoadedImage img;
    img.image_size = meta->total_load_memsz;

    // Segments are mapped writable, populated from the file, then restricted
    // to their final guest permissions: the guest W bit controls *guest*
    // stores, not the loader's own initialization writes.
    constexpr auto kLoaderMap = static_cast<memory::Prot>(
        static_cast<unsigned>(memory::Prot::R) | static_cast<unsigned>(memory::Prot::W) |
        static_cast<unsigned>(memory::Prot::X));

    auto map_and_copy = [&](uint64_t vaddr, const Phdr& p) -> std::expected<void, Error> {
        // The ELF spec requires only p_offset ≡ p_vaddr (mod page size), so a
        // segment's vaddr may sit mid-page (e.g. ld-placed .data segments).
        // Map the page-aligned base covering the whole segment and copy at
        // the exact vaddr — the same approach real program loaders use.
        constexpr uint64_t kPageSize = 0x1000;
        const uint64_t map_base = vaddr & ~(kPageSize - 1);
        const size_t map_size =
            static_cast<size_t>(vaddr - map_base + p.p_memsz);
        auto mapped = mem.map(map_base, map_size, kLoaderMap);
        if (!mapped)
            return std::unexpected(Error{mapped.error().code,
                                         "map PT_LOAD: " + mapped.error().message});
        if (p.p_filesz > 0) {
            auto w = mem.write(vaddr, data + p.p_offset, static_cast<size_t>(p.p_filesz));
            if (!w)
                return std::unexpected(Error{w.error().code,
                                             "copy PT_LOAD: " + w.error().message});
        }
        auto prot =
            mem.protect(map_base, map_size, flags_to_prot(p.p_flags));
        if (!prot)
            return std::unexpected(Error{prot.error().code,
                                         "protect PT_LOAD: " + prot.error().message});
        if (p.p_flags & elf::kPfX) mem.mark_exec(map_base, map_size);
        return {};
    };

    if (meta->type == elf::kTypeExec) {
        constexpr uint64_t kPageMask = 0xFFF;
        img.image_base = loads.empty() ? 0 : (loads[0].p_vaddr & ~kPageMask);
        for (const Phdr& p : loads) {
            auto r = map_and_copy(p.p_vaddr, p);
            if (!r) return std::unexpected(r.error());
            img.segments.push_back(
                LoadedSegment{p.p_vaddr, static_cast<size_t>(p.p_memsz), p.p_flags});
        }
    } else {
        // ET_DYN: reserve an image-sized hole at a deterministic base, then
        // map and slide every segment into it.
        constexpr uint64_t kBaseAlign = 0x100000; // 1 MiB
        auto base = mem.reserve_range(static_cast<size_t>(meta->total_load_memsz), kBaseAlign);
        if (!base)
            return std::unexpected(Error{base.error().code,
                                         "reserve image: " + base.error().message});
        img.image_base = *base;
        for (const Phdr& p : loads) {
            const uint64_t vaddr = *base + p.p_vaddr;
            auto r = map_and_copy(vaddr, p);
            if (!r) return std::unexpected(r.error());
            img.segments.push_back(
                LoadedSegment{vaddr, static_cast<size_t>(p.p_memsz), p.p_flags});
        }
    }

    // Entry must resolve inside one of the mapped segments.
    const uint64_t entry =
        meta->type == elf::kTypeExec ? h->e_entry : img.image_base + h->e_entry;
    bool entry_ok = false;
    for (const auto& s : img.segments) {
        if (entry >= s.vaddr && entry < s.vaddr + s.memsz) entry_ok = true;
    }
    if (!entry_ok)
        return std::unexpected(Error{Status::ParseError, "entry point outside mapped image"});

    if (meta->gnu_stack_executable)
        ARMSX5_LOG_WARN(TAG, "executable marks its stack executable (PT_GNU_STACK X)");

    img.entry = entry;
    ARMSX5_LOG_INFO(TAG, "loaded ELF: type=%u loads=%u entry=0x%llx base=0x%llx size=0x%llx",
                    meta->type, meta->load_count, static_cast<unsigned long long>(img.entry),
                    static_cast<unsigned long long>(img.image_base),
                    static_cast<unsigned long long>(img.image_size));
    return img;
}

} // namespace armsx5::loader
