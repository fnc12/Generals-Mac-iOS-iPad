#pragma once

#include "big_archive.h"

#include <filesystem>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace end_editor {

    // The files the game sees: loose files (under Data/, Art/ and Maps/) and .big archives of one
    // or more folders, merged.
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
        // Calls `visit` with the bytes of every copy of a path, the winning one first (for files
        // the game merges, like a mod's Terrain.ini over the base game's); one buffer is reused.
        // Returns the number of copies read.
        template<class F>
        std::size_t readEach(const std::string &path, F &&visit) const {
            std::size_t copies = 0;
            std::string bytes;
            if(this->read(path, bytes)) {
                ++copies;
                visit(std::as_const(bytes));
            }
            const auto range = this->shadowed.equal_range(path);
            for(auto it = range.first; it != range.second; ++it) {
                if(this->readSource(it->second, UINT32_MAX, bytes)) {
                    ++copies;
                    visit(std::as_const(bytes));
                }
            }
            return copies;
        }
        bool readPrefix(const std::string &path, std::uint32_t maxBytes, std::string &out) const;

        // The path with its original case and '/' separators ("Maps/Alpine Assault/Alpine Assault.map").
        std::string originalPath(const std::string &path) const;
        // Where the winning copy is: the archive's file name, or the loose file's full path.
        std::string sourceOf(const std::string &path) const;

        // Calls `visit` with every known path, in path order.
        template<class F>
        void forEachPath(F &&visit) const {
            for(const auto &item: this->sources) {
                visit(item.first);
            }
        }

      private:
        struct Source {
            int archive = -1;  // index in `archives`, or -1 for a loose file
            BigEntry entry;
            std::filesystem::path looseFile;
            std::string relativeName;  // loose files: path under the folder, original case
        };

        std::vector<BigArchive> archives;
        void add(const std::string &path, Source source);
        bool readSource(const Source &source, std::uint32_t maxBytes, std::string &out) const;

        std::map<std::string, Source> sources;
        std::multimap<std::string, Source> shadowed;  // copies that lost, in mount order
    };
}
