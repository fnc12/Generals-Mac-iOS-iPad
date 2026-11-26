#include "map_document.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

namespace end_editor {

    namespace {
        void putU16(std::string &out, std::uint16_t value) {
            out.append(reinterpret_cast<const char *>(&value), 2);
        }
        void putU32(std::string &out, std::uint32_t value) {
            out.append(reinterpret_cast<const char *>(&value), 4);
        }
        void putFloat(std::string &out, float value) {
            out.append(reinterpret_cast<const char *>(&value), 4);
        }
        void putAscii(std::string &out, const std::string &text) {
            putU16(out, std::uint16_t(std::min<std::size_t>(text.size(), 0xFFFF)));
            out.append(text, 0, std::min<std::size_t>(text.size(), 0xFFFF));
        }
        // UTF-8 in, UTF-16 out (the game's UnicodeString has no surrogate pairs: those become U+FFFD).
        void putUnicode(std::string &out, const std::string &text) {
            std::vector<std::uint16_t> units;
            for(std::size_t i = 0; i < text.size();) {
                const auto c = std::uint8_t(text[i]);
                std::uint32_t code = 0xFFFD;
                std::size_t length = 1;
                if(c < 0x80) {
                    code = c;
                } else if((c >> 5) == 0x6 && i + 1 < text.size()) {
                    code = ((c & 0x1Fu) << 6) | (std::uint8_t(text[i + 1]) & 0x3Fu);
                    length = 2;
                } else if((c >> 4) == 0xE && i + 2 < text.size()) {
                    code = ((c & 0x0Fu) << 12) | ((std::uint8_t(text[i + 1]) & 0x3Fu) << 6) |
                           (std::uint8_t(text[i + 2]) & 0x3Fu);
                    length = 3;
                } else if((c >> 3) == 0x1E) {
                    length = 4;
                }
                units.push_back(std::uint16_t(code > 0xFFFF || (code >= 0xD800 && code <= 0xDFFF) ? 0xFFFD : code));
                i += length;
            }
            putU16(out, std::uint16_t(std::min<std::size_t>(units.size(), 0xFFFF)));
            for(std::size_t i = 0; i < units.size() && i < 0xFFFF; ++i) {
                putU16(out, units[i]);
            }
        }

        // An angle in [-pi, pi), like MapObject::normalizeAngle.
        float normalizeAngle(float angle) {
            constexpr float pi = 3.14159265358979f;
            if(!std::isfinite(angle)) {
                return 0;
            }
            angle = std::fmod(angle + pi, 2 * pi);
            if(angle < 0) {
                angle += 2 * pi;
            }
            return angle - pi;
        }

        // The number after the last space of a uniqueID ("AmericaTankCrusader 12" -> 12), or -1.
        int uniqueNumber(const std::string &id) {
            const auto space = id.rfind(' ');
            if(space == std::string::npos || space + 1 >= id.size()) {
                return -1;
            }
            int value = 0;
            for(auto i = space + 1; i < id.size(); ++i) {
                if(id[i] < '0' || id[i] > '9' || value > 100000000) {
                    return -1;
                }
                value = value * 10 + (id[i] - '0');
            }
            return value;
        }

        DictValue boolean(const char *key, bool value) {
            DictValue v;
            v.type = DictValue::boolean;
            v.key = key;
            v.intValue = value ? 1 : 0;
            return v;
        }
        DictValue integer(const char *key, std::int32_t value) {
            DictValue v;
            v.type = DictValue::integer;
            v.key = key;
            v.intValue = value;
            return v;
        }
        DictValue ascii(const char *key, const std::string &text) {
            DictValue v;
            v.type = DictValue::ascii;
            v.key = key;
            v.text = text;
            return v;
        }

        // MapObject's defaults for a new object (WorldHeightMap.cpp).
        Dict defaultProperties() {
            return {integer("objectInitialHealth", 100),  boolean("objectEnabled", true),
                    boolean("objectIndestructible", false), boolean("objectUnsellable", false),
                    boolean("objectPowered", true),          boolean("objectRecruitableAI", true),
                    boolean("objectTargetable", false)};
        }

        void setValue(Dict &dict, const DictValue &value) {
            for(auto &existing: dict) {
                if(existing.key == value.key) {
                    existing = value;
                    return;
                }
            }
            dict.push_back(value);
        }

        std::uint16_t chunkVersion(const std::string &chunk) {
            std::uint16_t version = 0;
            if(chunk.size() >= 6) {
                std::memcpy(&version, chunk.data() + 4, 2);
            }
            return version;
        }
    }

    std::vector<MapPlayer> mapPlayers(const MapFile &map) {
        std::vector<MapPlayer> players;
        for(const auto &side: map.sides) {
            MapPlayer player;
            if(const auto *name = findDictValue(side.player, "playerName")) {
                player.name = name->text;
            }
            if(const auto *display = findDictValue(side.player, "playerDisplayName")) {
                player.displayName = display->text;
            }
            if(const auto *color = findDictValue(side.player, "playerColor"); color && color->type == DictValue::integer) {
                player.color = std::uint32_t(color->intValue) & 0xFFFFFF;
            }
            player.neutral = player.name.empty();
            // SidesList.cpp: every player has a default team named "team" + its name.
            player.team = "team" + player.name;
            const bool hasTeam = std::any_of(map.teams.begin(), map.teams.end(), [&](const Dict &team) {
                const auto *teamName = findDictValue(team, "teamName");
                return teamName && teamName->text == player.team;
            });
            if(hasTeam) {
                players.push_back(std::move(player));
            }
        }
        return players;
    }

    std::vector<std::string> mapTeams(const MapFile &map) {
        std::vector<std::string> teams;
        for(const auto &team: map.teams) {
            if(const auto *name = findDictValue(team, "teamName")) {
                teams.push_back(name->text);
            }
        }
        return teams;
    }

    MapDocument::MapDocument(MapFile map) : file(std::move(map)) {
        for(const auto &[id, name]: this->file.names) {
            this->nextNameId = std::max(this->nextNameId, id + 1);
        }
        // Children of ObjectsList as they are, with the objects the engine keeps (in file order,
        // like MapFile::objects) given ids.
        std::vector<const MapObject *> byChunk(this->file.objectChunks.size(), nullptr);
        for(const auto &object: this->file.objects) {
            if(object.chunk < byChunk.size()) {
                byChunk[object.chunk] = &object;
            }
        }
        for(std::size_t i = 0; i < this->file.objectChunks.size(); ++i) {
            const auto &span = this->file.objectChunks[i];
            Object entry;
            entry.chunk = this->file.data.substr(span.header, span.end - span.header);
            if(byChunk[i]) {
                entry.id = this->nextId++;
                entry.object = *byChunk[i];
                if(const auto *unique = findDictValue(entry.object.properties, "uniqueID")) {
                    this->nextUniqueNumber = std::max(this->nextUniqueNumber, uniqueNumber(unique->text) + 1);
                }
                if(const auto *waypoint = findDictValue(entry.object.properties, "waypointID")) {
                    this->nextWaypointId = std::max(this->nextWaypointId, waypoint->intValue + 1);
                }
            }
            this->entries.push_back(std::move(entry));
        }
        this->waypointLinks = this->file.waypointLinks;
        // Links can name waypoints the map no longer has; a new waypoint must not revive them.
        for(const auto &link: this->waypointLinks) {
            this->nextWaypointId = std::max({this->nextWaypointId, link[0] + 1, link[1] + 1});
        }
    }

    const MapDocument::Object *MapDocument::find(std::int64_t id) const {
        const auto index = this->indexOf(id);
        return index < this->entries.size() && !this->entries[index].removed ? &this->entries[index] : nullptr;
    }

    std::size_t MapDocument::indexOf(std::int64_t id) const {
        // Ids grow with the entries, so the entry is at most at index id - 1.
        if(id <= 0) {
            return this->entries.size();
        }
        for(auto i = std::min(std::size_t(id), this->entries.size()); i-- > 0;) {
            if(this->entries[i].id == id) {
                return i;
            }
        }
        return this->entries.size();
    }

    MapDocument::Object *MapDocument::findShown(std::int64_t id) {
        return const_cast<Object *>(std::as_const(*this).find(id));
    }

    std::int32_t MapDocument::waypointId(std::int64_t id) const {
        const auto *entry = this->find(id);
        const auto *value = entry ? findDictValue(entry->object.properties, "waypointID") : nullptr;
        return value && value->type == DictValue::integer ? value->intValue : 0;
    }

    std::int64_t MapDocument::waypointObject(std::int32_t waypointId) const {
        for(const auto &entry: this->entries) {
            if(entry.id == 0 || entry.removed) {
                continue;
            }
            const auto *value = findDictValue(entry.object.properties, "waypointID");
            if(value && value->type == DictValue::integer && value->intValue == waypointId) {
                return entry.id;
            }
        }
        return 0;
    }

    std::int64_t MapDocument::roadPartner(std::int64_t id) const {
        const auto index = this->indexOf(id);
        if(!this->find(id)) {
            return 0;
        }
        const auto flags = this->entries[index].object.flags;
        const bool first = (flags & (mapObjectRoadPoint1 | mapObjectBridgePoint1)) != 0;
        const bool second = (flags & (mapObjectRoadPoint2 | mapObjectBridgePoint2)) != 0;
        if(!first && !second) {
            return 0;
        }
        const auto wanted = first ? (mapObjectRoadPoint2 | mapObjectBridgePoint2) : (mapObjectRoadPoint1 | mapObjectBridgePoint1);
        for(auto i = index;;) {
            if(first ? ++i >= this->entries.size() : i-- == 0) {
                return 0;
            }
            const auto &other = this->entries[i];
            if(other.removed || other.id == 0) {
                continue;
            }
            return (other.object.flags & wanted) ? other.id : 0;
        }
    }

    std::uint32_t MapDocument::nameId(const std::string &name) {
        for(const auto &[id, known]: this->file.names) {
            if(known == name) {
                return id;
            }
        }
        const auto id = this->nextNameId++;
        this->file.names[id] = name;
        this->addedNames.push_back(name);
        return id;
    }

    void MapDocument::begin() {
        if(this->depth++ == 0) {
            this->open = Edit{};
        }
    }

    void MapDocument::commit() {
        if(this->depth == 0 || --this->depth > 0) {
            return;
        }
        auto edit = std::move(this->open);
        this->open = Edit{};
        for(auto &change: edit.changes) {
            change.after = this->entries[change.index];
        }
        std::erase_if(edit.changes, [](const Change &change) {
            return change.before.removed == change.after.removed && change.before.chunk == change.after.chunk;
        });
        if(edit.links) {
            edit.linksAfter = this->waypointLinks;
            edit.links = edit.linksAfter != edit.linksBefore;
        }
        if(edit.changes.empty() && !edit.links) {
            return;
        }
        this->changed.clear();
        for(const auto &change: edit.changes) {
            this->changed.push_back(this->entries[change.index].id);
        }
        this->changedLinks = edit.links;
        ++this->editRevision;
        this->record(std::move(edit));
    }

    MapDocument::Object &MapDocument::touch(std::size_t index) {
        const bool seen = std::any_of(this->open.changes.begin(), this->open.changes.end(),
                                      [&](const Change &change) { return change.index == index; });
        if(!seen) {
            this->open.changes.push_back(Change{index, this->entries[index], {}});
        }
        return this->entries[index];
    }

    void MapDocument::touchLinks() {
        if(!this->open.links) {
            this->open.links = true;
            this->open.linksBefore = this->waypointLinks;
            const bool hasChunk = std::any_of(this->file.chunks.begin(), this->file.chunks.end(),
                                              [](const MapChunkSpan &chunk) { return chunk.name == "WaypointsList"; });
            if(!hasChunk) {
                this->nameId("WaypointsList");  // save() appends the chunk
            }
        }
    }

    std::string MapDocument::encodeObject(const MapObject &object) {
        // Object version 3: x y z angle, flags, template name, properties.
        std::string data;
        putFloat(data, object.x);
        putFloat(data, object.y);
        putFloat(data, object.z);
        putFloat(data, object.angle);
        putU32(data, std::uint32_t(object.flags));
        putAscii(data, object.templateName);
        putU16(data, std::uint16_t(object.properties.size()));
        for(const auto &value: object.properties) {
            putU32(data, (this->nameId(value.key) << 8) | std::uint32_t(value.type));
            switch(value.type) {
                case DictValue::boolean:
                    data.push_back(char(value.intValue ? 1 : 0));
                    break;
                case DictValue::integer:
                    putU32(data, std::uint32_t(value.intValue));
                    break;
                case DictValue::real:
                    putFloat(data, value.realValue);
                    break;
                case DictValue::ascii:
                    putAscii(data, value.text);
                    break;
                case DictValue::unicode:
                    putUnicode(data, value.text);
                    break;
            }
        }
        std::string chunk;
        putU32(chunk, this->nameId("Object"));
        putU16(chunk, 3);
        putU32(chunk, std::uint32_t(data.size()));
        return chunk + data;
    }

    std::string MapDocument::uniqueId(const std::string &name) {
        return name + " " + std::to_string(this->nextUniqueNumber++);
    }

    std::int64_t MapDocument::add(MapObject object) {
        Step step(*this);
        if(this->file.objectsVersion == 0) {
            this->nameId("ObjectsList");
        }
        Object entry;
        entry.id = this->nextId++;
        entry.object = std::move(object);
        entry.object.chunk = this->entries.size();
        entry.chunk = this->encodeObject(entry.object);
        entry.removed = true;  // the state before this edit
        const auto id = entry.id;
        this->entries.push_back(std::move(entry));
        this->touch(this->entries.size() - 1).removed = false;
        return id;
    }

    std::int64_t MapDocument::place(const std::string &templateName, float x, float y, float angle,
                                    const std::string &team, std::int32_t flags, float z, const Dict *properties) {
        MapObject object;
        object.x = x;
        object.y = y;
        object.z = isPlausibleObjectHeight(z) ? z : 0;
        object.angle = normalizeAngle(angle);
        object.flags = flags;
        object.templateName = templateName;
        if(properties) {
            object.properties = *properties;
        } else {
            // MapObject's defaults, the owner and a uniqueID like WorldBuilder's.
            object.properties = defaultProperties();
            object.properties.push_back(ascii("originalOwner", team));
        }
        setValue(object.properties, ascii("uniqueID", this->uniqueId(templateName)));
        return this->add(std::move(object));
    }

    std::int64_t MapDocument::placeWaypoint(float x, float y) {
        // WaypointTool: the next waypointID, "Waypoint <n>" made unique, the neutral team.
        const auto waypoint = this->nextWaypointId++;
        auto number = waypoint;
        const auto taken = [&](const std::string &name) {
            return std::any_of(this->entries.begin(), this->entries.end(), [&](const Object &entry) {
                const auto *value = findDictValue(entry.object.properties, "waypointName");
                return !entry.removed && value && value->text == name;
            });
        };
        while(taken("Waypoint " + std::to_string(number))) {
            ++number;
        }
        const auto name = "Waypoint " + std::to_string(number);
        MapObject object;
        object.x = x;
        object.y = y;
        object.templateName = waypointTemplate;
        object.properties = defaultProperties();
        object.properties.push_back(integer("waypointID", waypoint));
        object.properties.push_back(ascii("waypointName", name));
        object.properties.push_back(ascii("originalOwner", "team"));
        object.properties.push_back(ascii("uniqueID", this->uniqueId(name)));  // verifyValidUniqueID names waypoints so
        return this->add(std::move(object));
    }

    std::int64_t MapDocument::placeRoad(const std::string &roadType, bool bridge, float x0, float y0, float x1, float y1,
                                        std::int32_t cornerFlags) {
        // RoadTool: two objects of the road type, one after the other in the list (the engine pairs
        // them that way), on the ground, angle 0, owned by the neutral team.
        Step step(*this);
        const auto point = [&](float x, float y, std::int32_t flag) {
            MapObject object;
            object.x = x;
            object.y = y;
            object.templateName = roadType;
            object.flags = flag | (bridge ? 0 : cornerFlags & (mapObjectRoadCornerAngled | mapObjectRoadCornerTight));
            object.properties = defaultProperties();
            object.properties.push_back(ascii("originalOwner", "team"));
            object.properties.push_back(ascii("uniqueID", this->uniqueId(roadType)));
            return this->add(std::move(object));
        };
        const auto first = point(x0, y0, bridge ? mapObjectBridgePoint1 : mapObjectRoadPoint1);
        point(x1, y1, bridge ? mapObjectBridgePoint2 : mapObjectRoadPoint2);
        return first;
    }

    void MapDocument::setTransform(Object &entry, float x, float y, float angle) {
        entry.object.x = x;
        entry.object.y = y;
        entry.object.angle = angle;
        // Every Object version starts with x, y, z, angle (floats) after the chunk header.
        const auto at = DataChunkReader::headerBytes;
        if(entry.chunk.size() >= at + 16) {
            std::memcpy(&entry.chunk[at], &x, 4);
            std::memcpy(&entry.chunk[at + 4], &y, 4);
            std::memcpy(&entry.chunk[at + 12], &angle, 4);
        }
    }

    bool MapDocument::move(std::int64_t id, float x, float y) {
        const auto index = this->indexOf(id);
        if(!this->find(id) || !std::isfinite(x) || !std::isfinite(y)) {
            return false;
        }
        Step step(*this);
        auto &entry = this->touch(index);
        this->setTransform(entry, x, y, entry.object.angle);
        return true;
    }

    bool MapDocument::rotate(std::int64_t id, float angle) {
        const auto index = this->indexOf(id);
        if(!this->find(id)) {
            return false;
        }
        Step step(*this);
        auto &entry = this->touch(index);
        this->setTransform(entry, entry.object.x, entry.object.y, normalizeAngle(angle));
        return true;
    }

    bool MapDocument::setHeight(std::int64_t id, float z) {
        const auto index = this->indexOf(id);
        if(!this->find(id) || !std::isfinite(z) || !isPlausibleObjectHeight(z)) {
            return false;
        }
        Step step(*this);
        auto &entry = this->touch(index);
        entry.object.z = z;
        const auto at = DataChunkReader::headerBytes;
        if(chunkVersion(entry.chunk) == 3 && entry.chunk.size() >= at + 16) {
            std::memcpy(&entry.chunk[at + 8], &z, 4);
        } else {
            entry.chunk = this->encodeObject(entry.object);  // older versions have no height the engine reads
        }
        return true;
    }

    bool MapDocument::setProperty(std::int64_t id, const DictValue &value) {
        const auto index = this->indexOf(id);
        if(!this->find(id) || value.key.empty()) {
            return false;
        }
        const auto *existing = findDictValue(this->entries[index].object.properties, value.key);
        if(existing && existing->type == value.type && existing->intValue == value.intValue &&
           existing->realValue == value.realValue && existing->text == value.text) {
            return true;  // as it is: the bytes stay
        }
        Step step(*this);
        auto &entry = this->touch(index);
        setValue(entry.object.properties, value);
        entry.chunk = this->encodeObject(entry.object);
        return true;
    }

    bool MapDocument::removeProperty(std::int64_t id, const std::string &key) {
        const auto index = this->indexOf(id);
        if(!this->find(id) || !findDictValue(this->entries[index].object.properties, key)) {
            return false;
        }
        Step step(*this);
        auto &entry = this->touch(index);
        std::erase_if(entry.object.properties, [&](const DictValue &value) { return value.key == key; });
        entry.chunk = this->encodeObject(entry.object);
        return true;
    }

    bool MapDocument::remove(std::int64_t id) {
        const auto index = this->indexOf(id);
        if(!this->find(id)) {
            return false;
        }
        Step step(*this);
        if(const auto waypoint = this->waypointId(id); waypoint != 0) {
            const bool linked = std::any_of(this->waypointLinks.begin(), this->waypointLinks.end(), [&](const Link &link) {
                return link[0] == waypoint || link[1] == waypoint;
            });
            if(linked) {
                this->touchLinks();
                std::erase_if(this->waypointLinks,
                              [&](const Link &link) { return link[0] == waypoint || link[1] == waypoint; });
            }
        }
        // A road or bridge point goes with the other end of its segment.
        if(const auto partner = this->roadPartner(id); partner != 0) {
            this->touch(this->indexOf(partner)).removed = true;
        }
        this->touch(index).removed = true;
        return true;
    }

    bool MapDocument::toggleLink(std::int64_t from, std::int64_t to) {
        const auto a = this->waypointId(from), b = this->waypointId(to);
        if(a == 0 || b == 0 || a == b) {
            return false;
        }
        Step step(*this);
        this->touchLinks();
        const auto at = std::find(this->waypointLinks.begin(), this->waypointLinks.end(), Link{a, b});
        if(at != this->waypointLinks.end()) {
            this->waypointLinks.erase(at);
        } else {
            this->waypointLinks.push_back(Link{a, b});
        }
        return true;
    }

    void MapDocument::record(Edit edit) {
        if(this->done.size() < this->savedAt) {
            this->savedAtValid = false;  // the saved state can no longer be reached by redo
        }
        this->done.push_back(std::move(edit));
        this->undone.clear();
    }

    void MapDocument::apply(const Edit &edit, bool forward) {
        this->changed.clear();
        for(const auto &change: edit.changes) {
            this->entries[change.index] = forward ? change.after : change.before;
            this->changed.push_back(this->entries[change.index].id);
        }
        if(edit.links) {
            this->waypointLinks = forward ? edit.linksAfter : edit.linksBefore;
        }
        this->changedLinks = edit.links;
        ++this->editRevision;
    }

    bool MapDocument::undo() {
        if(this->done.empty() || this->depth > 0) {
            return false;
        }
        auto edit = std::move(this->done.back());
        this->done.pop_back();
        this->apply(edit, false);
        this->undone.push_back(std::move(edit));
        return true;
    }

    bool MapDocument::redo() {
        if(this->undone.empty() || this->depth > 0) {
            return false;
        }
        auto edit = std::move(this->undone.back());
        this->undone.pop_back();
        this->apply(edit, true);
        this->done.push_back(std::move(edit));
        return true;
    }

    void MapDocument::markSaved() {
        this->savedAt = this->done.size();
        this->savedAtValid = true;
    }

    std::string MapDocument::save() const {
        const auto &data = this->file.data;
        std::string out;
        out.reserve(data.size() + 4096);
        const bool linksChanged = this->waypointLinks != this->file.waypointLinks;
        const auto has = [&](const char *name) {
            return std::any_of(this->file.chunks.begin(), this->file.chunks.end(),
                               [&](const MapChunkSpan &chunk) { return chunk.name == name; });
        };
        const bool anyShown = std::any_of(this->entries.begin(), this->entries.end(),
                                          [](const Object &entry) { return !entry.removed; });
        // Table of contents: as it was, plus the added names the file uses now (an undone
        // placement leaves the file as it was).
        std::vector<std::string> used;
        for(const auto &name: this->addedNames) {
            bool needed = (name == "ObjectsList" && anyShown && !has("ObjectsList")) ||
                          (name == "WaypointsList" && linksChanged && !has("WaypointsList"));
            for(std::size_t i = this->file.objectChunks.size(); i < this->entries.size() && !needed; ++i) {
                needed = !this->entries[i].removed && name == "Object";
            }
            for(const auto &entry: this->entries) {
                if(needed) {
                    break;
                }
                // Original objects with names that are not in the table: only through edits.
                needed = !entry.removed && findDictValue(entry.object.properties, name) != nullptr;
            }
            if(needed) {
                used.push_back(name);
            }
        }
        if(used.empty()) {
            out.append(data, 0, this->file.tableEnd);
        } else {
            std::int32_t count = 0;
            std::memcpy(&count, data.data() + 4, 4);
            out.append("CkMp");
            putU32(out, std::uint32_t(count) + std::uint32_t(used.size()));
            out.append(data, 8, this->file.tableEnd - 8);
            for(const auto &name: used) {
                for(const auto &[id, known]: this->file.names) {
                    if(known == name) {
                        out.push_back(char(name.size()));
                        out.append(name);
                        putU32(out, id);
                        break;
                    }
                }
            }
        }
        const auto idOf = [&](const char *name) {
            for(const auto &[id, known]: this->file.names) {
                if(known == name) {
                    return id;
                }
            }
            return 0u;
        };

        // ObjectsList: the children kept, then the placed objects; bytes after the last child of
        // the original (none in the shipped maps) stay.
        const auto writeObjects = [&](std::uint32_t nameId, std::uint16_t version, std::string_view tail) {
            std::string children;
            for(const auto &entry: this->entries) {
                if(!entry.removed) {
                    children += entry.chunk;
                }
            }
            children += tail;
            putU32(out, nameId);
            putU16(out, version);
            putU32(out, std::uint32_t(children.size()));
            out += children;
        };
        const auto writeLinks = [&](std::uint32_t nameId, std::uint16_t version) {
            std::string body;
            putU32(body, std::uint32_t(this->waypointLinks.size()));
            for(const auto &link: this->waypointLinks) {
                putU32(body, std::uint32_t(link[0]));
                putU32(body, std::uint32_t(link[1]));
            }
            putU32(out, nameId);
            putU16(out, version);
            putU32(out, std::uint32_t(body.size()));
            out += body;
        };
        std::size_t at = this->file.tableEnd;
        bool wroteObjects = false, wroteLinks = false;
        for(const auto &chunk: this->file.chunks) {
            out.append(data, at, chunk.header - at);  // nothing, unless the file has gaps
            std::uint32_t nameId = 0;
            std::memcpy(&nameId, data.data() + chunk.header, 4);
            if(chunk.name == "ObjectsList" && !wroteObjects) {
                const auto childrenEnd = this->file.objectChunks.empty() ? chunk.header + DataChunkReader::headerBytes
                                                                         : this->file.objectChunks.back().end;
                writeObjects(nameId, chunk.version,
                             std::string_view(data).substr(childrenEnd, chunk.end - childrenEnd));
                wroteObjects = true;
            } else if(chunk.name == "WaypointsList" && linksChanged && !wroteLinks) {
                writeLinks(nameId, chunk.version);
                wroteLinks = true;
            } else {
                out.append(data, chunk.header, chunk.end - chunk.header);
            }
            at = chunk.end;
        }
        out.append(data, at, data.size() - at);
        if(!wroteObjects && anyShown) {
            // The map had no ObjectsList: one at the end, after SidesList as the engine needs
            // (add() put its name in the table).
            writeObjects(idOf("ObjectsList"), 3, {});
        }
        if(linksChanged && !has("WaypointsList")) {
            writeLinks(idOf("WaypointsList"), 1);  // K_WAYPOINTS_VERSION_1, last like WorldBuilder writes it
        }
        return out;
    }
}
