#include "archive.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <freetype/ftgzip.h>
#include <freetype/ftsystem.h>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace ftl {
namespace {
std::string archiveKey(std::string name) {
    // package_pkg_file_info (0x636340) and stricmp (0x64a1b0) fold ASCII only.
    for (char& character : name) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return name;
}

void* inflateAllocate(FT_Memory, long size) {
    return size > 0 ? std::malloc(static_cast<std::size_t>(size)) : nullptr;
}

void inflateFree(FT_Memory, void* block) {
    std::free(block);
}

void* inflateReallocate(FT_Memory, long, long size, void* block) {
    if (size <= 0) {
        std::free(block);
        return nullptr;
    }
    return std::realloc(block, static_cast<std::size_t>(size));
}
} // namespace

std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           p[3];
}

std::uint16_t be16(const unsigned char* p) {
    return std::uint16_t((unsigned(p[0]) << 8) | p[1]);
}

Archive::Archive(const std::filesystem::path& path) : path_(path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot_open_data");
    }
    const auto size = std::filesystem::file_size(path);
    std::array<unsigned char, 16> h{};
    if (!in.read(reinterpret_cast<char*>(h.data()), h.size())) {
        throw std::runtime_error("truncated_pkg_header");
    }
    if (std::string_view(reinterpret_cast<char*>(h.data()), 4) != "PKG\n") {
        throw std::runtime_error("invalid_pkg_magic");
    }
    auto count = be32(h.data() + 8);
    auto ns = be32(h.data() + 12);
    if (be16(h.data() + 4) != 16 || be16(h.data() + 6) != 20 || count > 1000000 ||
        ns > 64 * 1024 * 1024) {
        throw std::runtime_error("unsupported_pkg_layout");
    }
    const std::uint64_t start = 16 + std::uint64_t(count) * 20 + ns;
    if (start > size) {
        throw std::runtime_error("truncated_pkg_index");
    }
    Bytes index(std::size_t(count) * 20);
    std::string names(ns, '\0');
    if (!in.read(reinterpret_cast<char*>(index.data()), std::streamsize(index.size())) ||
        !in.read(names.data(), ns)) {
        throw std::runtime_error("truncated_pkg_index");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        auto p = index.data() + std::size_t(i) * 20;
        auto nf = be32(p + 4);
        auto offset = be32(p + 8);
        auto length = be32(p + 12);
        auto expanded = be32(p + 16);
        const bool compressed = ((nf >> 24) & 1U) != 0;
        auto no = nf & 0xffffff;
        auto end = names.find('\0', no);
        if (no >= ns || end == std::string::npos || offset < start ||
            std::uint64_t(offset) + (compressed ? length : expanded) > size) {
            throw std::runtime_error("invalid_pkg_entry");
        }
        // package_pkg_file_info636340 only inspects compression flag bit0.
        // resource_load_data6309e0 uses expanded size for uncompressed reads.
        if (!entries_
                 .emplace(archiveKey(names.substr(no, end - no)),
                          Entry{offset, length, expanded, compressed})
                 .second) {
            throw std::runtime_error("duplicate_pkg_entry");
        }
    }
}

Bytes Archive::read(const std::string& name) const {
    const auto it = entries_.find(archiveKey(name));
    if (it == entries_.end()) {
        throw std::runtime_error("missing_asset: " + name);
    }
    const auto& e = it->second;
    if ((e.compressed && e.length > 64 * 1024 * 1024) || e.expanded > 64 * 1024 * 1024) {
        throw std::runtime_error("asset_too_large");
    }
    std::ifstream in(path_, std::ios::binary);
    in.seekg(e.offset);
    const auto storedLength = e.compressed ? e.length : e.expanded;
    Bytes bytes(storedLength);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), storedLength)) {
        throw std::runtime_error("truncated_asset: " + name);
    }
    if (e.compressed) {
        // zlib_decompress_to650d50 uses checked zlib inflate. FreeType's bundled
        // inflater also accepts gzip, so require the original zlib wrapper.
        if (bytes.size() < 2 || (bytes[0] & 15U) != 8U || (bytes[0] >> 4) > 7U ||
            ((unsigned(bytes[0]) << 8) | bytes[1]) % 31U != 0U || (bytes[1] & 32U) != 0U) {
            throw std::runtime_error("invalid_compressed_asset: " + name);
        }
        FT_MemoryRec_ memory{nullptr, inflateAllocate, inflateFree, inflateReallocate};
        Bytes expanded(std::max<std::size_t>(1, e.expanded), 0);
        FT_ULong decodedLength = static_cast<FT_ULong>(expanded.size());
        const auto error = FT_Gzip_Uncompress(&memory,
                                              expanded.data(),
                                              &decodedLength,
                                              bytes.data(),
                                              static_cast<FT_ULong>(bytes.size()));
        if (error != 0 || decodedLength > e.expanded) {
            throw std::runtime_error("invalid_compressed_asset: " + name);
        }
        // The original retains authored data_size even for a shorter stream.
        // Keep that size, with a defined zero tail instead of uninitialized bytes.
        expanded.resize(e.expanded);
        return expanded;
    }
    return bytes;
}

bool Archive::contains(const std::string& name) const {
    return entries_.contains(archiveKey(name));
}
} // namespace ftl
