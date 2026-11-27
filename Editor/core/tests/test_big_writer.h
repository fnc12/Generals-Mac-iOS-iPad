#pragma once

// Writes small BIG archives for tests, so no game data is needed.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace end_editor_tests {

    inline void appendBigEndian32(std::string& out, std::uint32_t value) {
        out.push_back(char(value >> 24));
        out.push_back(char(value >> 16));
        out.push_back(char(value >> 8));
        out.push_back(char(value));
    }

    // files: (path inside the archive, contents)
    inline void writeBig(const std::filesystem::path& path,
                         const std::vector<std::pair<std::string, std::string>>& files) {
        std::uint32_t indexEnd = 16;
        for (const auto& [name, body]: files) {
            indexEnd += 8 + std::uint32_t(name.size()) + 1;
        }
        std::string header = "BIGF";
        header.append(4, '\0');  // archive size, not read
        appendBigEndian32(header, std::uint32_t(files.size()));
        appendBigEndian32(header, indexEnd);
        std::uint32_t offset = indexEnd;
        std::string bodies;
        for (const auto& [name, body]: files) {
            appendBigEndian32(header, offset);
            appendBigEndian32(header, std::uint32_t(body.size()));
            header += name;
            header.push_back('\0');
            offset += std::uint32_t(body.size());
            bodies += body;
        }
        std::ofstream file(path, std::ios::binary);
        file << header << bodies;
    }

    // A fresh empty folder under the temp directory.
    inline std::filesystem::path makeTempFolder(const std::string& name) {
        const auto path = std::filesystem::temp_directory_path() / ("end-editor-tests-" + name);
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
        return path;
    }
}
