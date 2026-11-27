#pragma once

#include "map_file.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace end_editor {

    // An owner an object of the map can have, and the team the editor gives the objects it owns: a
    // player of the map's SidesList, or a start position of a multiplayer map.
    struct MapPlayer {
        std::string name;  // playerName ("" for the neutral player)
        std::string displayName;  // playerDisplayName
        std::string team;  // its default team: "team" + playerName
        std::uint32_t color = 0xFFFFFF;
        bool neutral = false;
        // A start position (1.., name "Start<n>", team "teamStart<n>"), 0 a player of the SidesList.
        // A multiplayer map's SidesList has no players of the game: GeneralsX's engine gives an
        // object whose originalOwner is "teamStart<n>" to the player who starts at start position
        // n, and leaves it out when nobody starts there (GameLogic.cpp, StartPositionOwner.h).
        int start = 0;
    };
    // The SidesList players that have their default team, then the start positions
    // (mapStartPositions) on a map of two or more.
    std::vector<MapPlayer> mapPlayers(const MapFile& map);
    // Every team of the SidesList (teamName), in file order, then the start positions' teams.
    std::vector<std::string> mapTeams(const MapFile& map);
    // The map's start positions, like the game's map cache counts them: the waypoints
    // "Player_1_Start", "Player_2_Start".. up to the first number missing, at most 8. A map of two
    // or more is a multiplayer map (Play starts it as a skirmish).
    int mapStartPositions(const MapFile& map);
    // The start position a waypoint marks: n for a waypoint named "Player_<n>_Start" (1..8), else 0.
    int startPositionOf(const MapObject& object);
    // The start position whose objects a team is: n for "teamStart<n>" (1..8), else 0.
    int startPositionOfTeam(std::string_view team);
    // The editor's color (0xRRGGBB) of start position n (1..8): its waypoint's label, its owner in the
    // owner menus and the objects it owns. The game has none (whoever starts there brings theirs).
    std::uint32_t startPositionColor(int start);

    // The template name of waypoints (WorldBuilder's WaypointTool).
    inline constexpr const char* waypointTemplate = "*Waypoints/Waypoint";

    // An open map that can be edited and written back: objects placed, moved, turned, raised,
    // given other properties and removed, waypoints linked, with undo and redo; everything else
    // of the file is kept byte for byte.
    //
    // Each child of the ObjectsList chunk is kept as its original bytes. A move or a turn rewrites
    // the position and angle inside them (the same fields in every Object version), a height
    // change of a version 3 object its z; an object given other properties (or a height while it
    // is an older version) is written again as an Object version 3 chunk, like a placed one. A
    // removed object is left out when saving. The table of contents only grows: names new chunks
    // need are appended to it. WaypointsList is written again only when its links changed.
    class MapDocument {
      public:
        struct Object {
            std::int64_t id = 0;  // 1.. for the objects the engine keeps; 0 for chunks it drops
            MapObject object;
            std::string chunk;  // the whole chunk: header and data
            bool removed = false;
        };
        // A link between two waypoints, by their waypointID.
        using Link = std::array<std::int32_t, 2>;

        explicit MapDocument(MapFile map);

        // The file as read; its `objects` are moved into objects() (map().objects is empty).
        const MapFile& map() const {
            return this->file;
        }
        // Every child of ObjectsList in file order, then placed objects. Shown: id != 0 and not removed.
        const std::vector<Object>& objects() const {
            return this->entries;
        }
        const Object* find(std::int64_t id) const;
        const std::vector<Link>& links() const {
            return this->waypointLinks;
        }
        // The waypointID of a shown waypoint, 0 when the object is none.
        std::int32_t waypointId(std::int64_t id) const;
        // The shown waypoint with this waypointID, 0 when there is none.
        std::int64_t waypointObject(std::int32_t waypointId) const;
        // The other end of a road or bridge segment: after a POINT1 the next shown object if it is
        // a POINT2, before a POINT2 the previous shown one if it is a POINT1 (how the engine pairs
        // them). 0 when the object is no road point or has no partner.
        std::int64_t roadPartner(std::int64_t id) const;
        // Grows with every edit, undo and redo.
        std::int64_t revision() const {
            return this->editRevision;
        }

        // Every edit between begin() and commit() is one undo step (groups nest; the outermost
        // commit records it). An edit outside a group is a step of its own.
        void begin();
        void commit();

        // Places a new object at a world position (z above the ground), owned by `team`. Returns
        // its id. With `properties` (a copy) it takes those instead of MapObject's defaults and the
        // owner, and gets a new uniqueID.
        std::int64_t place(const std::string& templateName,
                           float x,
                           float y,
                           float angle,
                           const std::string& team,
                           std::int32_t flags = 0,
                           float z = 0,
                           const Dict* properties = nullptr);
        // A new waypoint ("Waypoint <n>" with the next waypointID), like WorldBuilder's.
        std::int64_t placeWaypoint(float x, float y);
        // A road or bridge segment: two objects of the road type, the first with the POINT1 flag,
        // the second with POINT2, owned by the neutral team. Returns the first id (the second is +1).
        std::int64_t placeRoad(const std::string& roadType,
                               bool bridge,
                               float x0,
                               float y0,
                               float x1,
                               float y1,
                               std::int32_t cornerFlags = 0);
        // Each returns false when there is no such (shown) object or nothing would change.
        bool move(std::int64_t id, float x, float y);
        bool rotate(std::int64_t id, float angle);
        bool setHeight(std::int64_t id, float z);  // above the ground
        bool setProperty(std::int64_t id, const DictValue& value);  // replaces the key's value or adds it
        bool removeProperty(std::int64_t id, const std::string& key);
        // Removes the object, and the links of a waypoint.
        bool remove(std::int64_t id);
        // Adds the link from one waypoint to another, or removes it when it is there.
        bool toggleLink(std::int64_t from, std::int64_t to);

        bool undo();
        bool redo();
        std::size_t undoCount() const {
            return this->done.size();
        }
        std::size_t redoCount() const {
            return this->undone.size();
        }
        // The objects the last edit, undo or redo changed, and whether it changed links.
        const std::vector<std::int64_t>& lastChanged() const {
            return this->changed;
        }
        bool lastChangedLinks() const {
            return this->changedLinks;
        }
        std::int64_t lastChangedId() const {
            return this->changed.empty() ? 0 : this->changed.back();
        }
        // Whether there are edits since opening or the last save.
        bool edited() const {
            return this->done.size() != this->savedAt || this->savedAtValid == false;
        }

        // The file with the edits: uncompressed (the game reads both), same chunk versions.
        std::string save() const;
        void markSaved();

      private:
        struct Change {
            std::size_t index = 0;  // in entries
            Object before, after;
        };
        struct Edit {
            std::vector<Change> changes;
            bool links = false;
            std::vector<Link> linksBefore, linksAfter;
        };
        // Starts a group for one edit call unless one is open; commits it when it ends.
        struct Step {
            explicit Step(MapDocument& document) : document(document) {
                document.begin();
            }
            ~Step() {
                document.commit();
            }
            MapDocument& document;
        };

        Object* findShown(std::int64_t id);
        std::size_t indexOf(std::int64_t id) const;
        Object& touch(std::size_t index);
        void touchLinks();
        std::uint32_t nameId(const std::string& name);
        std::string encodeObject(const MapObject& object);
        std::string uniqueId(const std::string& name);
        std::int64_t add(MapObject object);
        void setTransform(Object& object, float x, float y, float angle);
        void apply(const Edit& edit, bool forward);
        void record(Edit edit);

        MapFile file;
        std::vector<Object> entries;
        std::vector<Link> waypointLinks;
        std::vector<std::string> addedNames;  // appended to the table of contents, in id order
        std::uint32_t nextNameId = 1;
        std::int64_t nextId = 1;
        int nextUniqueNumber = 0;
        std::int32_t nextWaypointId = 1;
        std::vector<Edit> done, undone;
        int depth = 0;
        Edit open;  // the group being built
        std::size_t savedAt = 0;
        bool savedAtValid = true;  // false when the saved state was undone past and edited over
        std::vector<std::int64_t> changed;
        bool changedLinks = false;
        std::int64_t editRevision = 0;
    };
}
