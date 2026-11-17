#include "armsx5/system/vfs.h"

#include "armsx5/core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace armsx5::system {
namespace {
constexpr const char* TAG = "VFS";
} // namespace

MemFile::MemFile(std::vector<uint8_t> initial) : data_(std::move(initial)) {}

size_t MemFile::size() const { return data_.size(); }

std::expected<size_t, Error> MemFile::read_at(uint64_t offset, uint8_t* dst, size_t len) {
    if (offset > data_.size())
        return std::unexpected(Error{Status::InvalidArgument, "read beyond EOF"});
    const size_t n = std::min<uint64_t>(len, data_.size() - offset);
    std::memcpy(dst, data_.data() + offset, n);
    return n;
}

std::expected<size_t, Error> MemFile::write_at(uint64_t offset, const uint8_t* src, size_t len) {
    if (offset + len > data_.size()) data_.resize(static_cast<size_t>(offset) + len);
    std::memcpy(data_.data() + offset, src, len);
    return len;
}

std::shared_ptr<MemFile> MemFS::put(const std::string& path, std::vector<uint8_t> data) {
    auto f = std::make_shared<MemFile>(std::move(data));
    files_[path] = f;
    return f;
}

std::expected<std::shared_ptr<IFile>, Error> MemFS::open(const std::string& path) const {
    auto it = files_.find(path);
    if (it == files_.end())
        return std::unexpected(Error{Status::NotFound, "no such file: " + path});
    return it->second;
}

std::vector<std::string> MemFS::list() const {
    std::vector<std::string> out;
    for (const auto& [k, v] : files_) out.push_back(k);
    return out;
}

HostDirectoryFS::HostDirectoryFS(std::string root) : root_(std::move(root)) {}

std::expected<HostDirectoryFS, Error> HostDirectoryFS::create(const std::string& root) {
    struct stat st{};
    if (::stat(root.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return std::unexpected(Error{Status::InvalidArgument, "root is not a directory: " + root});
    return HostDirectoryFS(root);
}

std::expected<std::shared_ptr<IFile>, Error> HostDirectoryFS::open(const std::string& path,
                                                                   bool for_write) {
    // Reject anything that could escape the root: absolute paths, ".."
    // components, double slashes, and NUL bytes are all invalid here.
    if (path.empty() || path.front() == '/')
        return std::unexpected(Error{Status::InvalidArgument, "absolute paths rejected"});
    if (path.find('\0') != std::string::npos)
        return std::unexpected(Error{Status::InvalidArgument, "NUL byte in path"});
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t slash = path.find('/', start);
        const std::string part = slash == std::string::npos ? path.substr(start)
                                                            : path.substr(start, slash - start);
        if (part == "..")
            return std::unexpected(Error{Status::InvalidArgument, "path traversal rejected"});
        if (!part.empty() && part != ".") parts.push_back(part);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string full = root_;
    for (const auto& p : parts) full += "/" + p;

    struct stat st{};
    if (::stat(full.c_str(), &st) != 0)
        return std::unexpected(Error{Status::NotFound, "no such file: " + path});
    if (!S_ISREG(st.st_mode))
        return std::unexpected(Error{Status::InvalidArgument, "not a regular file: " + path});
    if (for_write && access(full.c_str(), W_OK) != 0)
        return std::unexpected(Error{Status::IoError, "not writable: " + path});

    // Return a file-view shim over an fd-backed implementation.
    class HostFile final : public IFile {
    public:
        HostFile(std::string path, size_t size) : path_(std::move(path)), size_(size) {}
        ~HostFile() override {
            if (fd_ >= 0) ::close(fd_);
        }
        size_t size() const override { return size_; }
        std::expected<size_t, Error> read_at(uint64_t offset, uint8_t* dst, size_t len) override {
            if (fd_ < 0) {
                fd_ = ::open(path_.c_str(), O_RDONLY);
                if (fd_ < 0)
                    return std::unexpected(Error{Status::IoError, std::strerror(errno)});
            }
            const ssize_t n = ::pread(fd_, dst, len, static_cast<off_t>(offset));
            if (n < 0) return std::unexpected(Error{Status::IoError, std::strerror(errno)});
            return static_cast<size_t>(n);
        }
        std::expected<size_t, Error> write_at(uint64_t offset, const uint8_t* src,
                                              size_t len) override {
            if (fd_ < 0) {
                fd_ = ::open(path_.c_str(), O_WRONLY);
                if (fd_ < 0)
                    return std::unexpected(Error{Status::IoError, std::strerror(errno)});
            }
            const ssize_t n = ::pwrite(fd_, src, len, static_cast<off_t>(offset));
            if (n < 0) return std::unexpected(Error{Status::IoError, std::strerror(errno)});
            size_ = std::max(size_, static_cast<size_t>(offset) + len);
            return static_cast<size_t>(n);
        }

    private:
        std::string path_;
        size_t size_;
        int fd_ = -1;
    };
    return std::static_pointer_cast<IFile>(std::make_shared<HostFile>(full, st.st_size));
}

} // namespace armsx5::system
