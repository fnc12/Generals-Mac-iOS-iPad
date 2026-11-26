#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace end_editor {

    // One file inside a .big archive. `path` is normalized: lower case, '/' separators
    // ("data/ini/object/americavehicle.ini").
    struct BigEntry {
        std::string path;
        std::string name;  // as the archive writes it ("Maps\\Alpine Assault\\Alpine Assault.map")
        std::uint32_t offset = 0;
        std::uint32_t size = 0;
    };

    // Reader of the BIG archive index ("BIGF" / "BIG4" header, big-endian entry table).
    // Only the index is kept in memory; file bodies are read on demand.
    class BigArchive {
      public:
        // Returns nullopt and fills `error` when the file is not a valid archive.
        static std::optional<BigArchive> open(const std::filesystem::path &path, std::string &error);

        const std::filesystem::path &path() const {
            return this->filePath;
        }
        const std::vector<BigEntry> &entries() const {
            return this->fileEntries;
        }

        // Reads one entry's bytes. Returns false on an I/O error.
        bool read(const BigEntry &entry, std::string &out) const;
        // Reads at most the first `maxBytes` bytes of an entry.
        bool readPrefix(const BigEntry &entry, std::uint32_t maxBytes, std::string &out) const;

      private:
        std::filesystem::path filePath;
        std::vector<BigEntry> fileEntries;
    };

    // "Data\\INI\\Object\\X.ini" -> "data/ini/object/x.ini"
    std::string normalizeArchivePath(std::string_view path);
}
