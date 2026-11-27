#pragma once

// Builds small .map files for tests (the engine's DataChunk layout, see DataChunk.cpp and
// WorldBuilder's save code) and RefPack-compresses them, so no game data is needed. The
// contents are made up.

#include "test_w3d_writer.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace end_editor_tests {

    // Chunks with a name table, like DataChunkOutput.
    class ChunkWriter {
      public:
        Bytes body;

        std::uint32_t id(const std::string& name) {
            auto [it, added] = this->ids.emplace(name, std::uint32_t(this->ids.size() + 1));
            return it->second;
        }

        ChunkWriter& open(const std::string& name, std::uint16_t version) {
            this->body.u32(this->id(name)).u16(version).u32(0);
            this->starts.push_back(this->body.data.size());
            return *this;
        }

        ChunkWriter& close() {
            const auto start = this->starts.back();
            this->starts.pop_back();
            const auto size = std::uint32_t(this->body.data.size() - start);
            std::memcpy(&this->body.data[start - 4], &size, 4);
            return *this;
        }

        ChunkWriter& ascii(const std::string& text) {
            this->body.u16(std::uint16_t(text.size())).append(text);
            return *this;
        }

        // Dict entries: key, type (0 bool, 1 int, 2 real, 3 ascii, 4 unicode) and value.
        struct Entry {
            std::string key;
            int type;
            std::int32_t intValue = 0;
            float realValue = 0;
            std::string text;
        };
        ChunkWriter& dict(const std::vector<Entry>& entries) {
            this->body.u16(std::uint16_t(entries.size()));
            for (const auto& entry: entries) {
                this->body.u32((this->id(entry.key) << 8) | std::uint32_t(entry.type));
                switch (entry.type) {
                    case 0:
                        this->body.u8(std::uint8_t(entry.intValue));
                        break;
                    case 1:
                        this->body.u32(std::uint32_t(entry.intValue));
                        break;
                    case 2:
                        this->body.f32(entry.realValue);
                        break;
                    case 3:
                        this->ascii(entry.text);
                        break;
                    default:  // ASCII text written as UTF-16
                        this->body.u16(std::uint16_t(entry.text.size()));
                        for (const char c: entry.text) {
                            this->body.u16(std::uint16_t(std::uint8_t(c)));
                        }
                        break;
                }
            }
            return *this;
        }

        std::string file() const {
            Bytes out;
            out.append("CkMp").u32(std::uint32_t(this->ids.size()));
            for (const auto& [name, value]: this->ids) {
                out.u8(std::uint8_t(name.size())).append(name).u32(value);
            }
            return out.data + this->body.data;
        }

      private:
        std::map<std::string, std::uint32_t> ids;
        std::vector<std::size_t> starts;
    };

    // A greedy RefPack encoder that uses every command form, wrapped in "EAR\0" + size.
    inline std::string refPack(const std::string& input) {
        std::string out = "EAR";
        out.push_back('\0');
        const auto size = std::uint32_t(input.size());
        out.append(reinterpret_cast<const char*>(&size), 4);
        out.push_back(char(0x10));
        out.push_back(char(0xFB));
        out.push_back(char(size >> 16));
        out.push_back(char(size >> 8));
        out.push_back(char(size));
        std::size_t pending = 0;  // first literal not written yet
        const auto flushLiterals = [&](std::size_t upTo) {
            while (upTo - pending >= 4) {  // blocks of 4..112, leaving 0..3 for the next command
                const std::size_t count = std::min<std::size_t>((upTo - pending) / 4 * 4, 112);
                out.push_back(char(0xE0 | ((count - 4) >> 2)));
                out.append(input, pending, count);
                pending += count;
            }
        };
        std::size_t at = 0;
        while (at < input.size()) {
            std::size_t bestLength = 0, bestDistance = 0;
            for (std::size_t from = at > 131072 ? at - 131072 : 0; from < at; ++from) {
                std::size_t length = 0;
                while (at + length < input.size() && length < 1028 && input[from + length] == input[at + length]) {
                    ++length;
                }
                if (length > bestLength) {
                    bestLength = length;
                    bestDistance = at - from;
                }
            }
            const std::size_t offset = bestDistance - 1;
            const bool shortForm = bestLength >= 3 && bestLength <= 10 && offset < 1024;
            const bool mediumForm = bestLength >= 4 && bestLength <= 67 && offset < 16384;
            const bool longForm = bestLength >= 5;
            if (!shortForm && !mediumForm && !longForm) {
                ++at;
                continue;
            }
            flushLiterals(at);
            const auto literals = unsigned(at - pending);
            if (shortForm) {
                out.push_back(char(((offset >> 8) << 5) | ((bestLength - 3) << 2) | literals));
                out.push_back(char(offset & 0xFF));
            } else if (mediumForm) {
                out.push_back(char(0x80 | (bestLength - 4)));
                out.push_back(char((literals << 6) | (offset >> 8)));
                out.push_back(char(offset & 0xFF));
            } else {
                out.push_back(char(0xC0 | ((offset >> 16) << 4) | (((bestLength - 5) >> 8) << 2) | literals));
                out.push_back(char((offset >> 8) & 0xFF));
                out.push_back(char(offset & 0xFF));
                out.push_back(char((bestLength - 5) & 0xFF));
            }
            out.append(input, pending, literals);
            at += bestLength;
            pending = at;
        }
        flushLiterals(input.size());
        const auto last = unsigned(input.size() - pending);
        out.push_back(char(0xFC | last));
        out.append(input, pending, last);
        return out;
    }

    // A 5 x 4 vertex map with a border of 1: two texture classes, a blended and a cliff cell, a
    // tree, a waypoint, a water area, one player with a team, lighting, and a chunk the editor
    // does not know. `startWaypoints` > 1 adds the waypoints "Player_2_Start".. after the first
    // one: a multiplayer map.
    inline std::string testMap(int startWaypoints = 1) {
        ChunkWriter w;
        w.open("HeightMapData", 4);
        w.body.u32(5).u32(4).u32(1);  // width, height, border
        w.body.u32(1).u32(3).u32(2);  // one boundary: 3 x 2 cells
        w.body.u32(20);
        for (int i = 0; i < 20; ++i) {
            w.body.u8(std::uint8_t(i * 2));  // height grows to the north-east
        }
        w.close();

        w.open("BlendTileData", 8);
        w.body.u32(20);
        for (int i = 0; i < 20; ++i) {  // tiles: class "TestGrass" (tile 0) everywhere but cell 1
            w.body.u16(std::uint16_t(i == 1 ? (2 << 2) | 1 : 0));
        }
        for (int i = 0; i < 20; ++i) {  // blends: cell 6 lays blend tile 1 over its grass
            w.body.u16(std::uint16_t(i == 6 ? 1 : 0));
        }
        for (int i = 0; i < 20; ++i) {  // extra blends
            w.body.u16(0);
        }
        for (int i = 0; i < 20; ++i) {  // cliff infos: cell 7
            w.body.u16(std::uint16_t(i == 7 ? 1 : 0));
        }
        w.body.data.append(4 * 1, '\0');  // passability bits: 4 rows of (5 + 7) / 8 bytes
        w.body.u32(5).u32(2).u32(2);  // bitmap tiles, blend tiles, cliff infos
        w.body.u32(2);  // texture classes
        w.body.u32(0).u32(1).u32(1).u32(0);
        w.ascii("TestGrass");
        w.body.u32(1).u32(4).u32(2).u32(0);
        w.ascii("TestRock");
        w.body.u32(0).u32(0);  // edge tiles, edge classes
        // Blend tile 1: rock tile 3, horizontal, inverted.
        w.body.u32(3 << 2).u8(1).u8(0).u8(0).u8(0).u8(1).u8(0).u32(0xFFFFFFFFu).u32(0x7ADA0000);
        // Cliff info 1: on grass, stretched corners.
        w.body.u32(0).floats({0, -0.03125f, 0.015625f, -0.03125f, 0.015625f, 0, 0, 0}).u8(1).u8(0);
        w.close();

        w.open("WorldInfo", 1);
        w.dict({{"mapName", 3, 0, 0, "Test"}, {"weather", 1, 1}});
        w.close();

        w.open("SidesList", 3);
        w.body.u32(1);
        w.dict({{"playerName", 3, 0, 0, "PlyrRed"}, {"playerColor", 1, std::int32_t(0xFFFF0000u)}});
        w.body.u32(0);  // build list
        w.body.u32(1);
        w.dict({{"teamName", 3, 0, 0, "teamPlyrRed"}, {"teamOwner", 3, 0, 0, "PlyrRed"}});
        w.open("PlayerScriptsList", 1);
        w.body.u32(12345);  // scripts are not read
        w.close();
        w.close();

        w.open("ObjectsList", 3);
        w.open("Object", 3);
        w.body.floats({15, 5, 2, 1.5f}).u32(0);
        w.ascii("TestTree");
        w.dict({{"originalOwner", 3, 0, 0, "teamPlyrRed"},
                {"objectInitialHealth", 1, 100},
                {"objectScale", 2, 0, 1.5f},
                {"objectIndestructible", 0, 1},
                {"objectName", 4, 0, 0, "Oak"}});
        w.close();
        w.open("Object", 3);
        w.body.floats({5, 15, 0, 0}).u32(0);
        w.ascii("*Waypoints/Waypoint");
        w.dict({{"waypointID", 1, 1}, {"waypointName", 3, 0, 0, "Player_1_Start"}});
        w.close();
        for (int n = 2; n <= startWaypoints; ++n) {
            w.open("Object", 3);
            w.body.floats({5.0f + float(n), 15, 0, 0}).u32(0);
            w.ascii("*Waypoints/Waypoint");
            w.dict({{"waypointID", 1, std::int32_t(n)},
                    {"waypointName", 3, 0, 0, "Player_" + std::to_string(n) + "_Start"}});
            w.close();
        }
        w.close();

        w.open("PolygonTriggers", 4);
        w.body.u32(2);
        w.ascii("Water Area 1");
        w.ascii("");
        w.body.u32(1).u8(1).u8(0).u32(0).u32(4);
        for (const auto& point: std::vector<std::array<int, 2>>{{0, 0}, {20, 0}, {20, 10}, {0, 10}}) {
            w.body.u32(std::uint32_t(point[0])).u32(std::uint32_t(point[1])).u32(7);
        }
        w.ascii("Not water");
        w.ascii("");
        w.body.u32(2).u8(0).u8(0).u32(0).u32(3);
        w.body.u32(0).u32(0).u32(0).u32(5).u32(0).u32(0).u32(5).u32(5).u32(0);
        w.close();

        w.open("GlobalLighting", 3);
        w.body.u32(2);  // afternoon
        for (int time = 0; time < 4; ++time) {
            for (int light = 0; light < 6; ++light) {  // terrain, object, 2 object, 2 terrain
                const float level = time == 1 && light == 0 ? 0.5f : 0.25f;
                w.body.floats({level, level, level, 0.75f, 0.75f, 0.75f, 0, 0, -1});
            }
        }
        w.body.u32(0x7F000000u);
        w.close();

        w.open("MapPreview", 1);
        w.body.u32(1).u32(2).u32(3);
        w.close();
        return w.file();
    }
}
