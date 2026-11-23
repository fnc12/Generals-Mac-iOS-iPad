#pragma once

#include "big_archive.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace end_editor {

    // The files the game sees: loose files and .big archives of one or more folders, merged.
    // Paths are normalized ("art/w3d/avtank.w3d"). Who wins when a path exists more than once:
    //   - an earlier folder wins over a later one (Zero Hour is mounted before base Generals);
    //   - inside a folder a loose file (under Data/ or Art/) wins over an archived one;
    //   - among the archives of a folder the first in lower-case name order wins
    //     (that is why patch archives are named "!..." or "00...").
    class GameFiles {
      public:
        // Replaces what was mounted. Unreadable archives become warnings. Returns false when
        // the first folder does not exist.
        bool mount(const std::vector<std::filesystem::path> &folders, std::vector<std::string> &warnings,
                   std::string &error);

        int archiveCount() const {
            return int(this->archives.size());
        }
        bool exists(const std::string &path) const {
            return this->sources.count(path) != 0;
        }
        bool read(const std::string &path, std::string &out) const;
        bool readPrefix(const std::string &path, std::uint32_t maxBytes, std::string &out) const;

        // All known paths for which `keep` returns true, in path order.
        std::vector<std::string> list(const std::function<bool(const std::string &)> &keep) const;

      private:
        struct Source {
            int archive = -1;  // index in `archives`, or -1 for a loose file
            BigEntry entry;
            std::filesystem::path looseFile;
        };

        std::vector<BigArchive> archives;
        std::map<std::string, Source> sources;
    };
}
