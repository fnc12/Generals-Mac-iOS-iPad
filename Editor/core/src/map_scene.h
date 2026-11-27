#pragma once

#include "game_files.h"
#include "ini_objects.h"
#include "map_file.h"
#include "terrain_ini.h"
#include "texture.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace end_editor {

    // One terrain vertex as the client's renderer takes it (56 bytes, no padding). Every cell has
    // its own four vertices because neighbours show different pieces of the atlas.
    struct TerrainVertex {
        float position[3];
        float normal[3];
        float uv[2];  // base texture, atlas coordinates (top-left origin)
        float blendUv[2];  // texture blended over it
        float extraUv[2];  // third texture (3-way blends)
        float alpha[2];  // weight of the blend and the extra texture at this corner
    };
    static_assert(sizeof(TerrainVertex) == 56);

    // Cells per side of a terrain chunk; the store keeps one row per chunk so an edit can replace
    // only the chunks it touches.
    constexpr int terrainChunkCells = 32;

    struct TerrainChunk {
        int x = 0, y = 0;  // first cell
        std::vector<TerrainVertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    // A water area or river: a flat (rivers: per-point height) triangulated polygon.
    struct WaterArea {
        std::string name;
        bool river = false;
        float height = 0;
        std::vector<float> vertices;  // x y z, world units
        std::vector<std::uint32_t> indices;
    };

    // A map object resolved against the game's object templates (its properties stay in the
    // MapObject).
    struct PlacedObject {
        std::string templateName;
        // "model": drawn with its template's model; "marker": no model (or none found);
        // "waypoint", "road", "bridge": editor markers.
        std::string kind;
        float x = 0, y = 0, z = 0;  // z: world height (ground + the object's offset)
        float angle = 0;
        std::int32_t flags = 0;
        std::string model;  // the template's model, "" for markers
        // What its Draw module hides while idle (with the model, what the model is loaded with).
        std::string muzzleFlashes;
        std::string hiddenSubObjects;
        std::string owner;  // team name (originalOwner)
        std::uint32_t color = 0xFFFFFF;  // owner's player color, 0xRRGGBB
    };

    struct MapScene {
        Image atlas;  // every texture class of the map, 64 px per tile
        std::vector<TerrainChunk> chunks;
        std::vector<WaterArea> water;
        WaterLook waterLook;
        Image waterTexture;
        std::array<float, 4> playable{};  // world rectangle x0 y0 x1 y1 of the first boundary
        // Average color (RGB 0..1) of each source tile by tile number (MapFile::tiles >> 2), what
        // WorldHeightMap::getTerrainColorAt gives the map preview; black for tiles of no class.
        std::vector<std::array<float, 3>> tileColors;
        std::vector<std::string> warnings;
    };

    using ObjectLookup = std::function<const ObjectInfo*(const std::string& templateName)>;

    // Builds what the client draws: terrain meshes with atlas coordinates and blend weights
    // (WorldHeightMap::getAlphaUVData's per-corner alphas) and water polygons. Terrain.ini and
    // Water.ini are read from `files`. The objects are resolved one by one (resolveObject()).
    MapScene buildMapScene(const MapFile& map, const GameFiles& files);

    // One object as the scene shows it: kind, model, world height, owner color.
    PlacedObject resolveObject(const MapFile& map, const MapObject& object, const ObjectLookup& findObject);

    // Ear clipping of a simple polygon (either winding). Falls back to a fan for what it cannot
    // clip (self-intersecting outlines).
    std::vector<std::uint32_t> triangulatePolygon(const std::vector<std::array<float, 2>>& points);

    // Halves the image (box filter) until both sides are at most `maxSize`.
    Image shrinkImage(Image image, std::uint32_t maxSize);

    // Reads a texture from Art/Textures (or another folder): `name` with its extension, the .dds
    // twin preferred like the game does. Returns false when it is missing or unreadable.
    bool readGameTexture(const GameFiles& files,
                         const std::string& folder,
                         const std::string& name,
                         Image& out,
                         std::string& error);
}
