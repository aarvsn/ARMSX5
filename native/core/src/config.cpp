#include "armsx5/core/config.h"

#include "armsx5/core/log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <fstream>
#include <string>

namespace armsx5 {
namespace {
constexpr const char* TAG = "Config";

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}
} // namespace

std::string Config::get_string(const std::string& key, const std::string& fallback) const {
    auto it = kv_.find(key);
    return it == kv_.end() ? fallback : it->second;
}

long long Config::get_int(const std::string& key, long long fallback) const {
    auto it = kv_.find(key);
    if (it == kv_.end()) return fallback;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(it->second.c_str(), &end, 10);
    if (end == it->second.c_str() || errno == ERANGE) {
        ARMSX5_LOG_WARN(TAG, "non-integer value for key '%s' = '%s', using fallback %lld",
                        key.c_str(), it->second.c_str(), fallback);
        return fallback;
    }
    return v;
}

bool Config::get_bool(const std::string& key, bool fallback) const {
    auto it = kv_.find(key);
    if (it == kv_.end()) return fallback;
    std::string v = it->second;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    ARMSX5_LOG_WARN(TAG, "non-boolean value for key '%s' = '%s', using fallback %d",
                    key.c_str(), it->second.c_str(), fallback ? 1 : 0);
    return fallback;
}

void Config::set_string(const std::string& key, const std::string& value) { kv_[key] = value; }

void Config::set_int(const std::string& key, long long value) { kv_[key] = std::to_string(value); }

void Config::set_bool(const std::string& key, bool value) { kv_[key] = value ? "true" : "false"; }

bool Config::contains(const std::string& key) const { return kv_.count(key) != 0; }

std::vector<std::string> Config::keys() const {
    std::vector<std::string> out;
    out.reserve(kv_.size());
    for (const auto& [k, v] : kv_) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

bool Config::load_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        ARMSX5_LOG_WARN(TAG, "cannot open config for reading: %s", path.c_str());
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (!key.empty()) kv_[key] = value;
    }
    return true;
}

bool Config::save_file(const std::string& path) const {
    std::ofstream out(path);
    if (!out) {
        ARMSX5_LOG_ERROR(TAG, "cannot open config for writing: %s", path.c_str());
        return false;
    }
    for (const auto& k : keys()) out << k << " = " << kv_.at(k) << "\n";
    return static_cast<bool>(out);
}

} // namespace armsx5
