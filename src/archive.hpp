#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ftl {
using Bytes = std::vector<unsigned char>;
std::uint32_t be32(const unsigned char* p);
std::uint16_t be16(const unsigned char* p);

class Archive {
    struct Entry {
        std::uint32_t offset, length, expanded;
        bool compressed;
    };

    std::filesystem::path path_;
    std::unordered_map<std::string, Entry> entries_;

public:
    explicit Archive(const std::filesystem::path& path);
    Bytes read(const std::string& name) const;

    bool contains(const std::string& name) const;

    std::size_t count() const {
        return entries_.size();
    }
};
} // namespace ftl
