#pragma once
// Virtual filesystem abstraction.
//
// The guest never touches the host filesystem directly. HostDirectoryFS
// exposes one rooted directory with strict path-traversal protection
// (rejects "..", absolute escapes, symlink-escaping resolves); MemFS
// provides an in-memory backend for tests and CI. Android SAF integration
// plugs into the same interface (docs/architecture/system.md).
#include "armsx5/core/result.h"

#include <expected>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace armsx5::system {

class IFile {
public:
    virtual ~IFile() = default;
    virtual size_t size() const = 0;
    virtual std::expected<size_t, Error> read_at(uint64_t offset, uint8_t* dst, size_t len) = 0;
    virtual std::expected<size_t, Error> write_at(uint64_t offset, const uint8_t* src,
                                                  size_t len) = 0;
};

// In-memory file (test/CI backend).
class MemFile : public IFile {
public:
    explicit MemFile(std::vector<uint8_t> initial = {});
    size_t size() const override;
    std::expected<size_t, Error> read_at(uint64_t offset, uint8_t* dst, size_t len) override;
    std::expected<size_t, Error> write_at(uint64_t offset, const uint8_t* src, size_t len) override;
    std::vector<uint8_t>& data() { return data_; }

private:
    std::vector<uint8_t> data_;
};

// Rooted host directory backend with traversal protection.
class HostDirectoryFS {
public:
    // root must be an existing directory.
    static std::expected<HostDirectoryFS, Error> create(const std::string& root);

    // path is relative to the root; returns nullptr-status NotFound when the
    // file does not exist, InvalidArgument on traversal attempts.
    std::expected<std::shared_ptr<IFile>, Error> open(const std::string& path, bool for_write);

    const std::string& root() const { return root_; }

private:
    HostDirectoryFS(std::string root);
    std::string root_;
};

// Simple named-file registry used by tests and by the loader pipeline.
class MemFS {
public:
    std::shared_ptr<MemFile> put(const std::string& path, std::vector<uint8_t> data);
    std::expected<std::shared_ptr<IFile>, Error> open(const std::string& path) const;
    std::vector<std::string> list() const;

private:
    std::map<std::string, std::shared_ptr<MemFile>> files_;
};

} // namespace armsx5::system
