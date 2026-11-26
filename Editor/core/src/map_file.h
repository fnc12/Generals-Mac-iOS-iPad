#pragma once

#include "data_chunk.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace end_editor {

    // World units per height map cell and per height step (MapObject.h).
    constexpr float mapXYFactor = 10.0f;
    constexpr float mapHeightScale = mapXYFactor / 16.0f;

    // A texture class of BlendTileData: a square TGA of `width` x `width` tiles (64 px each) from
    // Terrain.ini, numbered firstTile.. in the map's tile list, rows from the image's bottom.
    struct MapTextureClass {
        std::int32_t firstTile = 0;
        std::int32_t numTiles = 0;
        std::int32_t width = 0;
        std::string name;  // Terrain.ini entry ("AsphaltType1")
    };

    // How a blend tile lays a second texture over a cell (TBlendTileInfo).
    struct MapBlendTile {
        std::int32_t tile = 0;  // tile index of the texture laid over (same encoding as MapFile::tiles)
        bool horizontal = false;
        bool vertical = false;
        bool rightDiagonal = false;
        bool leftDiagonal = false;
        std::uint8_t inverted = 0;  // bit 0: inverted, bit 1: forced flip (3-way blends)
        bool longDiagonal = false;
        std::int32_t customEdgeClass = -1;  // >= 0: drawn with an edge texture instead of an alpha ramp
    };

    // Texture coordinates of a cliff cell, stretched so steep slopes do not smear (TCliffInfo).
    // In units of the 2048-pixel atlas WorldBuilder had, relative to the class's bottom left.
    struct MapCliffInfo {
        std::int32_t tile = 0;
        std::array<float, 8> uv{};  // u0 v0 u1 v1 u2 v2 u3 v3, corners as in MapFile::tiles
        bool flip = false;
        bool mutant = false;
    };

    struct MapObject {
        float x = 0, y = 0, z = 0;  // z: height above the ground
        float angle = 0;            // radians about Z
        std::int32_t flags = 0;
        std::string templateName;
        Dict properties;
        std::size_t chunk = 0;  // its chunk in MapFile::objectChunks
    };

    // Where a chunk is in MapFile::data: its 10-byte header starts at `header`, its data ends at
    // `end`.
    struct MapChunkSpan {
        std::string name;
        std::uint16_t version = 0;
        std::size_t header = 0, end = 0;
    };
    // MapObject.h flags.
    constexpr std::int32_t mapObjectRoadFlags = 0x02 | 0x04;
    constexpr std::int32_t mapObjectBridgeFlags = 0x10 | 0x20;
    constexpr std::int32_t mapObjectDontRender = 0x100;

    struct MapPolygon {
        std::string name;
        std::string layer;
        std::int32_t id = 0;
        bool water = false;
        bool river = false;
        std::int32_t riverStart = 0;
        std::vector<std::array<std::int32_t, 3>> points;  // world units
    };

    struct MapLight {
        std::array<float, 3> ambient{}, diffuse{}, direction{0, 0, -1};
    };

    // Lighting of one time of day (1 morning .. 4 night): terrain and object light 0.
    struct MapLighting {
        std::int32_t timeOfDay = 2;
        std::array<MapLight, 4> terrain{}, objects{};
        std::uint32_t shadowColor = 0;
    };

    struct MapSide {
        Dict player;
        int buildListCount = 0;
    };

    // Everything the editor reads from a .map file. Heights and tiles are width x height,
    // rows from south (y = 0) to north.
    struct MapFile {
        std::int32_t width = 0, height = 0, border = 0;
        std::vector<std::array<std::int32_t, 2>> boundaries;  // playable areas, in cells from the border
        std::vector<std::uint8_t> heights;

        bool hasTiles = false;
        std::uint16_t blendVersion = 0;
        // Per cell: tile index (bits 2.. tile number, bit 0 east half, bit 1 north half of the 64 px
        // tile), blend tile (0 none, index into blendTiles), the 3-way blend tile, cliff info index.
        std::vector<std::int16_t> tiles, blends, extraBlends, cliffs;
        std::int32_t bitmapTileCount = 0;
        std::vector<MapTextureClass> textureClasses, edgeClasses;
        std::vector<MapBlendTile> blendTiles;  // [0] is unused, like the engine's
        std::vector<MapCliffInfo> cliffInfos;  // [0] is the default (no stretching)

        Dict worldInfo;
        std::vector<MapSide> sides;
        std::vector<Dict> teams;
        std::vector<MapObject> objects;
        std::vector<MapPolygon> polygons;
        bool hasLighting = false;
        MapLighting lighting;

        std::vector<std::string> chunkNames;  // top-level chunks, in file order
        std::vector<std::string> warnings;

        // What writing the file back needs (map_document.h): the unpacked bytes, where the table
        // of contents ends, its names, the top-level chunks and the children of ObjectsList (every
        // one, also those the engine drops). Chunks are copied from `data` as they are.
        std::string data;
        std::size_t tableEnd = 0;
        std::map<std::uint32_t, std::string> names;
        std::vector<MapChunkSpan> chunks;
        std::vector<MapChunkSpan> objectChunks;
        std::uint16_t objectsVersion = 0;  // of the ObjectsList chunk, 0 when the map has none
    };

    // Whether the engine keeps an object at this height above the ground (it drops the others
    // while reading the map).
    bool isPlausibleObjectHeight(float z);

    // Parses a .map file (compressed or not). Returns false and sets `error` when the file cannot
    // be read at all; damaged or unexpected parts that can be skipped become warnings.
    bool parseMap(std::string_view bytes, MapFile &map, std::string &error);

    // Height of the ground at a world position (bilinear over the height map, in world units).
    float groundHeight(const MapFile &map, float x, float y);
}
