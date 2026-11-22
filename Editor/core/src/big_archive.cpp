#include "big_archive.h"

#include <fstream>

namespace end_editor {

    namespace {
        std::uint32_t readBigEndian32(const unsigned char *bytes) {
            return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
                   (std::uint32_t(bytes[2]) << 8) | std::uint32_t(bytes[3]);
        }
    }

    std::string normalizeArchivePath(std::string_view path) {
        std::string result;
        result.reserve(path.size());
        for(char c: path) {
            if(c == '\\') {
                c = '/';
            } else if(c >= 'A' && c <= 'Z') {
                c = char(c - 'A' + 'a');
            }
            result.push_back(c);
        }
        return result;
    }

    std::optional<BigArchive> BigArchive::open(const std::filesystem::path &path, std::string &error) {
        std::ifstream file(path, std::ios::binary);
        if(!file) {
            error = "cannot open";
            return std::nullopt;
        }
        file.seekg(0, std::ios::end);
        const auto fileSize = std::uint64_t(file.tellg());
        file.seekg(0);

        // Header: magic[4], archive size (little endian, not used), entry count, index end
        // (both big endian).
        unsigned char header[16];
        if(!file.read(reinterpret_cast<char *>(header), sizeof(header))) {
            error = "too short";
            return std::nullopt;
        }
        const std::string_view magic(reinterpret_cast<const char *>(header), 4);
        if(magic != "BIGF" && magic != "BIG4") {
            error = "not a BIG archive";
            return std::nullopt;
        }
        const std::uint32_t count = readBigEndian32(header + 8);
        const std::uint32_t indexEnd = readBigEndian32(header + 12);
        if(indexEnd > fileSize || indexEnd < sizeof(header)) {
            error = "bad index size";
            return std::nullopt;
        }

        std::string index(indexEnd - sizeof(header), '\0');
        if(!file.read(index.data(), std::streamsize(index.size()))) {
            error = "cannot read index";
            return std::nullopt;
        }

        BigArchive archive;
        archive.filePath = path;
        archive.fileEntries.reserve(count);
        std::size_t position = 0;
        for(std::uint32_t i = 0; i < count; ++i) {
            if(position + 8 > index.size()) {
                error = "index ends early";
                return std::nullopt;
            }
            const auto *bytes = reinterpret_cast<const unsigned char *>(index.data() + position);
            BigEntry entry;
            entry.offset = readBigEndian32(bytes);
            entry.size = readBigEndian32(bytes + 4);
            position += 8;
            const auto nameEnd = index.find('\0', position);
            if(nameEnd == std::string::npos) {
                error = "unterminated file name";
                return std::nullopt;
            }
            entry.path = normalizeArchivePath(std::string_view(index).substr(position, nameEnd - position));
            position = nameEnd + 1;
            if(std::uint64_t(entry.offset) + entry.size > fileSize) {
                error = "entry outside the archive: " + entry.path;
                return std::nullopt;
            }
            archive.fileEntries.push_back(std::move(entry));
        }
        return archive;
    }

    bool BigArchive::read(const BigEntry &entry, std::string &out) const {
        std::ifstream file(this->filePath, std::ios::binary);
        if(!file) {
            return false;
        }
        file.seekg(entry.offset);
        out.resize(entry.size);
        return bool(file.read(out.data(), std::streamsize(entry.size)));
    }
}
