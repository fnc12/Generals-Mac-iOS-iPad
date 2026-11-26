#pragma once

#include "map_file.h"

#include <cstdint>
#include <string>
#include <vector>

namespace end_editor {

    // A player of the map's SidesList and the team the editor gives the objects it owns.
    struct MapPlayer {
        std::string name;         // playerName ("" for the neutral player)
        std::string displayName;  // playerDisplayName
        std::string team;         // its default team: "team" + playerName
        std::uint32_t color = 0xFFFFFF;
        bool neutral = false;
    };
    std::vector<MapPlayer> mapPlayers(const MapFile &map);

    // An open map that can be edited and written back. Objects can be placed, moved, turned and
    // removed, with undo and redo; everything else of the file is kept byte for byte.
    //
    // Each child of the ObjectsList chunk is kept as its original bytes. A move or a turn rewrites
    // the position and angle inside them (the same fields in every Object version), a placed object
    // gets new bytes (Object version 3, like WorldBuilder), a removed one is left out when saving.
    // The table of contents only grows: names a placed object needs are appended to it.
    class MapDocument {
      public:
        struct Object {
            std::int64_t id = 0;  // 1.. for the objects the engine keeps; 0 for chunks it drops
            MapObject object;
            std::string chunk;  // the whole chunk: header and data
            bool removed = false;
        };

        explicit MapDocument(MapFile map);

        const MapFile &map() const {
            return this->file;
        }
        // Every child of ObjectsList in file order, then placed objects. Shown: id != 0 and not removed.
        const std::vector<Object> &objects() const {
            return this->entries;
        }
        const Object *find(std::int64_t id) const;

        // Places a new object at a world position (on the ground), owned by `team`. Returns its id.
        std::int64_t place(const std::string &templateName, float x, float y, float angle, const std::string &team);
        // Each returns false when there is no such (shown) object.
        bool move(std::int64_t id, float x, float y);
        bool rotate(std::int64_t id, float angle);
        bool remove(std::int64_t id);

        bool undo();
        bool redo();
        std::size_t undoCount() const {
            return this->done.size();
        }
        std::size_t redoCount() const {
            return this->undone.size();
        }
        // The object the last edit, undo or redo changed.
        std::int64_t lastChanged() const {
            return this->changed;
        }
        // Whether there are edits since opening or the last save.
        bool edited() const {
            return this->done.size() != this->savedAt || this->savedAtValid == false;
        }

        // The file with the edits: uncompressed (the game reads both), same chunk versions.
        std::string save() const;
        void markSaved();

      private:
        struct Edit {
            enum Kind { place, remove, transform } kind = place;
            std::int64_t id = 0;
            float x0 = 0, y0 = 0, angle0 = 0;  // transform: before
            float x1 = 0, y1 = 0, angle1 = 0;  // transform: after
        };

        Object *findShown(std::int64_t id);
        std::uint32_t nameId(const std::string &name);
        void setTransform(Object &object, float x, float y, float angle);
        void apply(const Edit &edit, bool forward);
        void record(Edit edit);

        MapFile file;
        std::vector<Object> entries;
        std::vector<std::string> addedNames;  // appended to the table of contents, in id order
        std::uint32_t nextNameId = 1;
        std::int64_t nextId = 1;
        int nextUniqueNumber = 0;
        std::vector<Edit> done, undone;
        std::size_t savedAt = 0;
        bool savedAtValid = true;  // false when the saved state was undone past and edited over
        std::int64_t changed = 0;
    };
}
