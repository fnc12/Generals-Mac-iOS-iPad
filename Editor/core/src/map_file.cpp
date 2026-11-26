#include "map_file.h"

#include "compression.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// The readers follow the engine's: WorldHeightMap.cpp (HeightMapData, BlendTileData, ObjectsList,
// WorldInfo, GlobalLighting), PolygonTrigger.cpp and SidesList.cpp. Version numbers are those of
// MapReaderWriterInfo.h.

namespace end_editor {

    namespace {
        constexpr std::int32_t maxMapDimension = 8192;
        constexpr std::int32_t maxMapArea = 4096 * 4096;
        constexpr std::int32_t blendTileFlag = 0x7ADA0000;  // FLAG_VAL, ends every blend tile record

        std::vector<std::int16_t> readShorts(DataChunkReader &file, std::size_t count) {
            const auto bytes = file.readBytes(count * 2);
            std::vector<std::int16_t> result(count);
            std::memcpy(result.data(), bytes.data(), bytes.size());
            return result;
        }

        void parseHeightMap(DataChunkReader &file, const DataChunkReader::Chunk &chunk, MapFile &map) {
            map.width = file.readInt();
            map.height = file.readInt();
            map.border = chunk.version >= 3 ? file.readInt() : 0;
            if(map.width <= 0 || map.height <= 0 || map.width > maxMapDimension || map.height > maxMapDimension ||
               map.width * map.height > maxMapArea || map.border < 0 || map.border > (map.width - 1) / 2 ||
               map.border > (map.height - 1) / 2) {
                throw ChunkError("implausible map size");
            }
            map.boundaries.clear();
            if(chunk.version >= 4) {
                const auto count = file.readInt();
                if(count < 0 || count > 1000) {
                    throw ChunkError("implausible boundary count");
                }
                for(std::int32_t i = 0; i < count; ++i) {
                    const auto x = file.readInt();
                    const auto y = file.readInt();
                    map.boundaries.push_back({x, y});
                }
            } else {
                map.boundaries.push_back({map.width - 2 * map.border, map.height - 2 * map.border});
            }
            const auto size = file.readInt();
            if(size != map.width * map.height) {
                throw ChunkError("height data size does not match the map size");
            }
            const auto bytes = file.readBytes(std::size_t(size));
            map.heights.assign(bytes.begin(), bytes.end());
            if(chunk.version == 1) {  // cells of 5 units: keep every second sample
                const auto newWidth = (map.width + 1) / 2;
                const auto newHeight = (map.height + 1) / 2;
                std::vector<std::uint8_t> halved(std::size_t(newWidth * newHeight));
                for(std::int32_t y = 0; y < newHeight; ++y) {
                    for(std::int32_t x = 0; x < newWidth; ++x) {
                        halved[std::size_t(y * newWidth + x)] = map.heights[std::size_t(2 * y * map.width + 2 * x)];
                    }
                }
                map.heights = std::move(halved);
                map.width = newWidth;
                map.height = newHeight;
            }
        }

        void parseBlendTiles(DataChunkReader &file, const DataChunkReader::Chunk &chunk, MapFile &map) {
            const auto version = chunk.version;
            const auto size = file.readInt();
            if(map.heights.empty() || size != map.width * map.height) {
                throw ChunkError("tile data size does not match the height map");
            }
            const auto count = std::size_t(size);
            map.blendVersion = version;
            map.tiles = readShorts(file, count);
            map.blends = readShorts(file, count);
            map.extraBlends = version >= 6 ? readShorts(file, count) : std::vector<std::int16_t>(count, 0);
            map.cliffs = version >= 5 ? readShorts(file, count) : std::vector<std::int16_t>(count, 0);
            if(version >= 7) {
                // Passability bits per cell; the editor does not show them yet.
                const std::int32_t rowBytes = version == 7 ? (map.width + 1) / 8 : (map.width + 7) / 8;
                file.readBytes(std::size_t(map.height * rowBytes));
            }
            map.bitmapTileCount = file.readInt();
            const auto blendTileCount = file.readInt();
            const auto cliffInfoCount = version >= 5 ? file.readInt() : 1;
            const auto classCount = file.readInt();
            if(blendTileCount < 0 || blendTileCount > 100000 || cliffInfoCount < 0 || cliffInfoCount > 100000 ||
               classCount < 0 || classCount > 1000) {
                throw ChunkError("implausible tile counts");
            }
            map.textureClasses.clear();
            for(std::int32_t i = 0; i < classCount; ++i) {
                MapTextureClass textureClass;
                textureClass.firstTile = file.readInt();
                textureClass.numTiles = file.readInt();
                textureClass.width = file.readInt();
                file.readInt();  // legacy "isGDF"
                textureClass.name = file.readAsciiString();
                map.textureClasses.push_back(std::move(textureClass));
            }
            map.edgeClasses.clear();
            if(version >= 4) {
                file.readInt();  // edge tile count
                const auto edgeClassCount = file.readInt();
                if(edgeClassCount < 0 || edgeClassCount > 1000) {
                    throw ChunkError("implausible edge class count");
                }
                for(std::int32_t i = 0; i < edgeClassCount; ++i) {
                    MapTextureClass textureClass;
                    textureClass.firstTile = file.readInt();
                    textureClass.numTiles = file.readInt();
                    textureClass.width = file.readInt();
                    textureClass.name = file.readAsciiString();
                    map.edgeClasses.push_back(std::move(textureClass));
                }
            }
            map.blendTiles.assign(1, MapBlendTile{});
            for(std::int32_t i = 1; i < blendTileCount; ++i) {
                MapBlendTile tile;
                tile.tile = file.readInt();
                tile.horizontal = file.readByte() != 0;
                tile.vertical = file.readByte() != 0;
                tile.rightDiagonal = file.readByte() != 0;
                tile.leftDiagonal = file.readByte() != 0;
                tile.inverted = file.readByte();
                tile.longDiagonal = version >= 3 ? file.readByte() != 0 : false;
                tile.customEdgeClass = version >= 4 ? file.readInt() : -1;
                if(file.readInt() != blendTileFlag) {
                    throw ChunkError("blend tile " + std::to_string(i) + " has no end marker");
                }
                map.blendTiles.push_back(tile);
            }
            map.cliffInfos.assign(1, MapCliffInfo{});
            if(version >= 5) {
                for(std::int32_t i = 1; i < cliffInfoCount; ++i) {
                    MapCliffInfo info;
                    info.tile = file.readInt();
                    for(auto &value: info.uv) {
                        value = file.readReal();
                    }
                    info.flip = file.readByte() != 0;
                    info.mutant = file.readByte() != 0;
                    map.cliffInfos.push_back(info);
                }
            }
            // Like the engine, out-of-range indices become "none" (some shipped maps have them).
            int repaired = 0;
            const auto clamp = [&](std::vector<std::int16_t> &values, std::size_t limit) {
                for(auto &value: values) {
                    if(value < 0 || std::size_t(value) >= limit) {
                        value = 0;
                        ++repaired;
                    }
                }
            };
            clamp(map.blends, map.blendTiles.size());
            clamp(map.extraBlends, map.blendTiles.size());
            clamp(map.cliffs, map.cliffInfos.size());
            if(repaired > 0) {
                map.warnings.push_back("BlendTileData: " + std::to_string(repaired) + " blend or cliff indices out of range");
            }
            map.hasTiles = true;
        }

        void parseObjects(DataChunkReader &file, MapFile &map) {
            DataChunkReader::Chunk child;
            while(file.openChunk(child)) {
                map.objectChunks.push_back(
                    MapChunkSpan{child.name, child.version, child.start - DataChunkReader::headerBytes, child.end});
                if(child.name == "Object") {
                    MapObject object;
                    object.x = file.readReal();
                    object.y = file.readReal();
                    object.z = file.readReal();
                    if(child.version <= 2) {
                        object.z = 0;
                    }
                    object.angle = file.readReal();
                    object.flags = file.readInt();
                    object.templateName = file.readAsciiString();
                    if(child.version >= 2) {
                        object.properties = file.readDict();
                    }
                    object.chunk = map.objectChunks.size() - 1;
                    if(isPlausibleObjectHeight(object.z)) {
                        map.objects.push_back(std::move(object));
                    }
                }
                file.closeChunk();
            }
        }

        void parsePolygons(DataChunkReader &file, const DataChunkReader::Chunk &chunk, MapFile &map) {
            auto count = file.readInt();
            while(count-- > 0) {
                MapPolygon polygon;
                polygon.name = file.readAsciiString();
                if(chunk.version >= 4) {
                    polygon.layer = file.readAsciiString();
                }
                polygon.id = file.readInt();
                if(chunk.version >= 2) {
                    polygon.water = file.readByte() != 0;
                }
                if(chunk.version >= 3) {
                    polygon.river = file.readByte() != 0;
                    polygon.riverStart = file.readInt();
                }
                const auto pointCount = file.readInt();
                if(pointCount < 0 || std::size_t(pointCount) * 12 > file.left()) {
                    throw ChunkError("polygon " + polygon.name + " has an implausible point count");
                }
                for(std::int32_t i = 0; i < pointCount; ++i) {
                    const auto x = file.readInt();
                    const auto y = file.readInt();
                    const auto z = file.readInt();
                    polygon.points.push_back({x, y, z});
                }
                if(pointCount >= 2) {  // the engine deletes smaller ones
                    map.polygons.push_back(std::move(polygon));
                }
            }
        }

        void parseSides(DataChunkReader &file, const DataChunkReader::Chunk &chunk, MapFile &map) {
            const auto count = file.readInt();
            if(count < 0 || count > 1000) {
                throw ChunkError("implausible side count");
            }
            for(std::int32_t i = 0; i < count; ++i) {
                MapSide side;
                side.player = file.readDict();
                side.buildListCount = file.readInt();
                for(std::int32_t j = 0; j < side.buildListCount; ++j) {
                    file.readAsciiString();  // building name
                    file.readAsciiString();  // template
                    for(int k = 0; k < 4; ++k) {
                        file.readReal();  // x, y, z, angle
                    }
                    file.readByte();  // initially built
                    file.readInt();   // rebuilds
                    if(chunk.version >= 3) {
                        file.readAsciiString();  // script
                        file.readInt();          // health
                        file.readByte();         // whiner
                        file.readByte();         // unsellable
                        file.readByte();         // repairable
                    }
                }
                if(i < 16) {  // MAX_PLAYER_COUNT
                    map.sides.push_back(std::move(side));
                }
            }
            if(chunk.version >= 2) {
                const auto teamCount = file.readInt();
                if(teamCount < 0 || teamCount > 100000) {
                    throw ChunkError("implausible team count");
                }
                for(std::int32_t i = 0; i < teamCount; ++i) {
                    map.teams.push_back(file.readDict());
                }
            }
            // PlayerScriptsList child chunks follow; the editor does not read scripts yet.
        }

        void parseLighting(DataChunkReader &file, const DataChunkReader::Chunk &chunk, MapFile &map) {
            auto &lighting = map.lighting;
            lighting.timeOfDay = file.readInt();
            const auto readLight = [&](MapLight &light) {
                for(auto *values: {&light.ambient, &light.diffuse, &light.direction}) {
                    for(auto &value: *values) {
                        value = file.readReal();
                    }
                }
            };
            for(int i = 0; i < 4; ++i) {
                readLight(lighting.terrain[std::size_t(i)]);
                readLight(lighting.objects[std::size_t(i)]);
                MapLight extra;
                if(chunk.version >= 2) {  // two more object lights
                    readLight(extra);
                    readLight(extra);
                }
                if(chunk.version >= 3) {  // two more terrain lights
                    readLight(extra);
                    readLight(extra);
                }
            }
            if(!file.atEndOfChunk()) {
                lighting.shadowColor = std::uint32_t(file.readInt());
            }
            map.hasLighting = true;
        }
    }

    bool parseMap(std::string_view bytes, MapFile &map, std::string &error) {
        map = MapFile{};
        std::string data;
        if(!decompressGameData(bytes, data, error)) {
            return false;
        }
        try {
            DataChunkReader file(data);
            map.tableEnd = file.tableEnd();
            map.names = file.nameTable();
            DataChunkReader::Chunk chunk;
            while(file.openChunk(chunk)) {
                map.chunkNames.push_back(chunk.name);
                map.chunks.push_back(
                    MapChunkSpan{chunk.name, chunk.version, chunk.start - DataChunkReader::headerBytes, chunk.end});
                const auto depth = file.depth();
                try {
                    if(chunk.name == "HeightMapData") {
                        parseHeightMap(file, chunk, map);
                    } else if(chunk.name == "BlendTileData") {
                        parseBlendTiles(file, chunk, map);
                    } else if(chunk.name == "WorldInfo") {
                        map.worldInfo = file.readDict();
                    } else if(chunk.name == "SidesList") {
                        parseSides(file, chunk, map);
                    } else if(chunk.name == "ObjectsList" && map.objectsVersion == 0) {
                        map.objectsVersion = std::max<std::uint16_t>(chunk.version, 1);
                        parseObjects(file, map);
                    } else if(chunk.name == "PolygonTriggers") {
                        parsePolygons(file, chunk, map);
                    } else if(chunk.name == "GlobalLighting") {
                        parseLighting(file, chunk, map);
                    }
                    // Like the engine's "Unexpected data left over" asserts: a reader that stops
                    // early has misread the chunk. SidesList ends with script chunks it skips.
                    const bool known = chunk.name == "HeightMapData" || chunk.name == "BlendTileData" ||
                                       chunk.name == "WorldInfo" || chunk.name == "ObjectsList" ||
                                       chunk.name == "PolygonTriggers" || chunk.name == "GlobalLighting";
                    if(known && file.depth() == depth && !file.atEndOfChunk()) {
                        map.warnings.push_back(chunk.name + ": " + std::to_string(file.left()) + " bytes not read");
                    }
                } catch(const ChunkError &chunkError) {
                    map.warnings.push_back(chunk.name + ": " + chunkError.what());
                    if(chunk.name == "HeightMapData") {
                        map.heights.clear();
                    } else if(chunk.name == "BlendTileData") {
                        map.hasTiles = false;
                    }
                }
                file.closeChunksTo(depth);
                file.closeChunk();
            }
        } catch(const ChunkError &chunkError) {
            error = chunkError.what();
            return false;
        }
        if(map.heights.empty()) {
            error = "the map has no height map";
            return false;
        }
        map.data = std::move(data);
        if(!map.hasTiles) {
            map.warnings.push_back("no terrain tiles (BlendTileData)");
        }
        return true;
    }

    bool isPlausibleObjectHeight(float z) {
        // The engine drops objects far above or below the terrain.
        return z >= -100 * mapXYFactor && z <= 255 * 10 * mapHeightScale;
    }

    float groundHeight(const MapFile &map, float x, float y) {
        if(map.heights.empty()) {
            return 0;
        }
        const float fx = std::clamp(x / mapXYFactor + float(map.border), 0.0f, float(map.width - 1));
        const float fy = std::clamp(y / mapXYFactor + float(map.border), 0.0f, float(map.height - 1));
        const auto x0 = std::min(std::int32_t(fx), map.width - 2 < 0 ? 0 : map.width - 2);
        const auto y0 = std::min(std::int32_t(fy), map.height - 2 < 0 ? 0 : map.height - 2);
        const auto x1 = std::min(x0 + 1, map.width - 1);
        const auto y1 = std::min(y0 + 1, map.height - 1);
        const auto at = [&](std::int32_t i, std::int32_t j) {
            return float(map.heights[std::size_t(j * map.width + i)]) * mapHeightScale;
        };
        const float tx = fx - float(x0), ty = fy - float(y0);
        const float south = at(x0, y0) * (1 - tx) + at(x1, y0) * tx;
        const float north = at(x0, y1) * (1 - tx) + at(x1, y1) * tx;
        return south * (1 - ty) + north * ty;
    }
}
