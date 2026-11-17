#pragma once
// Key/value configuration store used by the Android frontend and native runtime.
// Format: "key = value" lines, '#' comments. Typed getters never throw; they
// return caller-provided defaults so missing configuration is always visible.
#include <string>
#include <unordered_map>
#include <vector>

namespace armsx5 {

class Config {
public:
    std::string get_string(const std::string& key, const std::string& fallback) const;
    long long get_int(const std::string& key, long long fallback) const;
    bool get_bool(const std::string& key, bool fallback) const;

    void set_string(const std::string& key, const std::string& value);
    void set_int(const std::string& key, long long value);
    void set_bool(const std::string& key, bool value);

    bool contains(const std::string& key) const;
    std::vector<std::string> keys() const;
    size_t size() const { return kv_.size(); }

    bool load_file(const std::string& path);   // false on IO error (file may not exist)
    bool save_file(const std::string& path) const;

private:
    std::unordered_map<std::string, std::string> kv_;
};

} // namespace armsx5
