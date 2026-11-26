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
            }
            this->entries.push_back(std::move(entry));
        }
    }

    const MapDocument::Object *MapDocument::find(std::int64_t id) const {
        for(const auto &entry: this->entries) {
            if(entry.id == id && id != 0) {
                return entry.removed ? nullptr : &entry;
            }
        }
        return nullptr;
    }

    MapDocument::Object *MapDocument::findShown(std::int64_t id) {
        return const_cast<Object *>(std::as_const(*this).find(id));
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

    std::int64_t MapDocument::place(const std::string &templateName, float x, float y, float angle,
                                    const std::string &team) {
        if(this->file.objectsVersion == 0) {
            this->nameId("ObjectsList");
        }
        Object entry;
        entry.id = this->nextId++;
        auto &object = entry.object;
        object.x = x;
        object.y = y;
        object.z = 0;  // on the ground
        object.angle = normalizeAngle(angle);
        object.templateName = templateName;
        // MapObject's defaults for a new object (WorldHeightMap.cpp), the owner and a uniqueID
        // like WorldBuilder's.
        const auto boolean = [](const char *key, bool value) {
            DictValue v;
            v.type = DictValue::boolean;
            v.key = key;
            v.intValue = value ? 1 : 0;
            return v;
        };
        DictValue health;
        health.type = DictValue::integer;
        health.key = "objectInitialHealth";
        health.intValue = 100;
        DictValue owner;
        owner.type = DictValue::ascii;
        owner.key = "originalOwner";
        owner.text = team;
        DictValue unique;
        unique.type = DictValue::ascii;
        unique.key = "uniqueID";
        unique.text = templateName + " " + std::to_string(this->nextUniqueNumber++);
        object.properties = {health,
                             boolean("objectEnabled", true),
                             boolean("objectIndestructible", false),
                             boolean("objectUnsellable", false),
                             boolean("objectPowered", true),
                             boolean("objectRecruitableAI", true),
                             boolean("objectTargetable", false),
                             owner,
                             unique};

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
                case DictValue::unicode:  // not used for new objects
                    putU16(data, 0);
                    break;
            }
        }
        putU32(entry.chunk, this->nameId("Object"));
        putU16(entry.chunk, 3);
        putU32(entry.chunk, std::uint32_t(data.size()));
        entry.chunk += data;
        entry.object.chunk = this->entries.size();

        const auto id = entry.id;
        this->entries.push_back(std::move(entry));
        this->record(Edit{Edit::place, id});
        this->changed = id;
        return id;
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
        auto *entry = this->findShown(id);
        if(!entry || !std::isfinite(x) || !std::isfinite(y)) {
            return false;
        }
        const auto &object = entry->object;
        Edit edit{Edit::transform, id, object.x, object.y, object.angle, x, y, object.angle};
        this->setTransform(*entry, x, y, object.angle);
        this->record(edit);
        this->changed = id;
        return true;
    }

    bool MapDocument::rotate(std::int64_t id, float angle) {
        auto *entry = this->findShown(id);
        if(!entry) {
            return false;
        }
        const auto &object = entry->object;
        angle = normalizeAngle(angle);
        Edit edit{Edit::transform, id, object.x, object.y, object.angle, object.x, object.y, angle};
        this->setTransform(*entry, object.x, object.y, angle);
        this->record(edit);
        this->changed = id;
        return true;
    }

    bool MapDocument::remove(std::int64_t id) {
        auto *entry = this->findShown(id);
        if(!entry) {
            return false;
        }
        entry->removed = true;
        this->record(Edit{Edit::remove, id});
        this->changed = id;
        return true;
    }

    void MapDocument::record(Edit edit) {
        if(this->done.size() < this->savedAt) {
            this->savedAtValid = false;  // the saved state can no longer be reached by redo
        }
        this->done.push_back(edit);
        this->undone.clear();
    }

    void MapDocument::apply(const Edit &edit, bool forward) {
        for(auto &entry: this->entries) {
            if(entry.id != edit.id) {
                continue;
            }
            switch(edit.kind) {
                case Edit::place:
                    entry.removed = !forward;
                    break;
                case Edit::remove:
                    entry.removed = forward;
                    break;
                case Edit::transform:
                    if(forward) {
                        this->setTransform(entry, edit.x1, edit.y1, edit.angle1);
                    } else {
                        this->setTransform(entry, edit.x0, edit.y0, edit.angle0);
                    }
                    break;
            }
            this->changed = edit.id;
            return;
        }
    }

    bool MapDocument::undo() {
        if(this->done.empty()) {
            return false;
        }
        const auto edit = this->done.back();
        this->done.pop_back();
        this->apply(edit, false);
        this->undone.push_back(edit);
        return true;
    }

    bool MapDocument::redo() {
        if(this->undone.empty()) {
            return false;
        }
        const auto edit = this->undone.back();
        this->undone.pop_back();
        this->apply(edit, true);
        this->done.push_back(edit);
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
        // Table of contents: as it was, plus the names placed objects added.
        if(this->addedNames.empty()) {
            out.append(data, 0, this->file.tableEnd);
        } else {
            std::int32_t count = 0;
            std::memcpy(&count, data.data() + 4, 4);
            out.append("CkMp");
            putU32(out, std::uint32_t(count) + std::uint32_t(this->addedNames.size()));
            out.append(data, 8, this->file.tableEnd - 8);
            for(const auto &name: this->addedNames) {
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
        std::size_t at = this->file.tableEnd;
        bool wroteObjects = false;
        for(const auto &chunk: this->file.chunks) {
            out.append(data, at, chunk.header - at);  // nothing, unless the file has gaps
            if(chunk.name == "ObjectsList" && !wroteObjects) {
                std::uint32_t nameId = 0;
                std::memcpy(&nameId, data.data() + chunk.header, 4);
                const auto childrenEnd = this->file.objectChunks.empty() ? chunk.header + DataChunkReader::headerBytes
                                                                         : this->file.objectChunks.back().end;
                writeObjects(nameId, chunk.version,
                             std::string_view(data).substr(childrenEnd, chunk.end - childrenEnd));
                wroteObjects = true;
            } else {
                out.append(data, chunk.header, chunk.end - chunk.header);
            }
            at = chunk.end;
        }
        out.append(data, at, data.size() - at);
        const bool anyShown = std::any_of(this->entries.begin(), this->entries.end(),
                                          [](const Object &entry) { return !entry.removed; });
        if(!wroteObjects && anyShown) {
            // The map had no ObjectsList: one at the end, after SidesList as the engine needs
            // (place() added its name).
            for(const auto &[id, known]: this->file.names) {
                if(known == "ObjectsList") {
                    writeObjects(id, 3, {});
                    break;
                }
            }
        }
        return out;
    }
}
