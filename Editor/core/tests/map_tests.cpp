#include "compression.h"
#include "data_chunk.h"
#include "editor_core.h"
#include "map_file.h"
#include "map_scene.h"
#include "test_big_writer.h"
#include "test_map_writer.h"
#include "test_w3d_writer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <random>

using namespace end_editor;
using namespace end_editor_tests;
using Catch::Approx;

namespace {
    // `count` 32-bit pixels of one color (TGA order: blue, green, red, alpha).
    std::vector<std::uint8_t> pixels(std::size_t count, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        std::vector<std::uint8_t> result;
        for (std::size_t i = 0; i < count; ++i) {
            result.insert(result.end(), {b, g, r, 255});
        }
        return result;
    }

    // A game folder with the test map (compressed, in an archive), a loose uncompressed copy,
    // the terrain textures, Terrain.ini, Water.ini and a tree object with a model.
    void makeMapGameFolder(const std::filesystem::path& folder) {
        const std::string tree = squareMesh("TRUNK", "TTREE", "TTree.tga") +
                                 hierarchy("TTREE",
                                           {TestPivot{"ROOTTRANSFORM", 0xFFFFFFFFu, {0, 0, 0}, {0, 0, 0, 1}},
                                            TestPivot{"TOP", 0, {0, 0, 10}, {0, 0, 0, 1}}}) +
                                 hlod("TTREE", "TTREE", {{1, "TTREE.TRUNK"}});
        writeBig(folder / "MapsZH.big",
                 {{"Maps\\Test Map\\Test Map.map", refPack(testMap())}, {"Maps\\Test Map\\map.ini", "; not a map\n"}});
        writeBig(folder / "TerrainZH.big",
                 {{"Art\\Terrain\\TGrass.tga", tga(64, 64, pixels(64 * 64, 0, 200, 0))},
                  {"Art\\Terrain\\TRock.tga", tga(128, 128, pixels(128 * 128, 120, 120, 120))}});
        writeBig(folder / "W3DZH.big",
                 {{"Art\\W3D\\TTree.w3d", tree},
                  {"Art\\Textures\\TTree.tga", tga(1, 1, {0, 128, 0, 255})},
                  {"Art\\Textures\\TWWater01.tga", tga(2, 2, pixels(4, 0, 0, 255))}});
        writeBig(folder / "INIZH.big",
                 {{"Data\\INI\\Terrain.ini",
                   "Terrain TestGrass\n  Texture = TGrass.tga\n  Class = GRASS\nEnd\n"
                   "Terrain TestRock\n  Texture = TRock.tga\nEnd\n"},
                  {"Data\\INI\\Water.ini",
                   "WaterSet AFTERNOON\n  WaterTexture = TSWater.tga\n"
                   "  DiffuseColor = R:255 G:255 B:255 A:255\n"
                   "  TransparentDiffuseColor = R:255 G:255 B:255 A:153\nEnd\n"
                   "WaterTransparency\n  StandingWaterColor = R:128 G:128 B:255\n"
                   "  StandingWaterTexture = TWWater01.tga\nEnd\n"},
                  {"Data\\INI\\Object\\Trees.ini",
                   "Object TestTree\n  Draw = W3DModelDraw M\n"
                   "    DefaultConditionState\n      Model = TTree\n    End\n  End\nEnd\n"}});
        std::filesystem::create_directories(folder / "Maps" / "Loose");
        std::ofstream(folder / "Maps" / "Loose" / "Loose.map", std::ios::binary) << testMap();
    }

    MapFile parsed(const std::string& bytes) {
        MapFile map;
        std::string error;
        REQUIRE(parseMap(bytes, map, error));
        CHECK(error.empty());
        return map;
    }
}

TEST_CASE("RefPack round trip") {
    std::mt19937 random(7);
    std::string input;
    for (int i = 0; i < 3000; ++i) {
        input.push_back(char('a' + random() % 3));  // short repeats: short and medium copies
    }
    input += std::string(2000, 'x');  // a long run: long copies
    for (int i = 0; i < 500; ++i) {
        input.push_back(char(random()));  // noise: literal blocks
    }
    input += input.substr(100, 1500);  // far copy
    const auto packed = refPack(input);
    CHECK(packed.size() < input.size());
    CHECK(isCompressedGameData(packed));

    std::string out, error;
    REQUIRE(decompressGameData(packed, out, error));
    CHECK(out == input);

    // Damaged streams fail instead of overrunning.
    for (const std::size_t cut: {std::size_t(9), packed.size() / 2, packed.size() - 1}) {
        std::string broken = packed.substr(0, cut);
        CHECK_FALSE(decompressGameData(broken, out, error));
    }
    std::string wrongSize = packed;
    wrongSize[4] = char(wrongSize[4] + 1);
    CHECK_FALSE(decompressGameData(wrongSize, out, error));

    // Not compressed: copied as is.
    REQUIRE(decompressGameData("CkMp1234", out, error));
    CHECK(out == "CkMp1234");
}

TEST_CASE("Map chunks") {
    for (const bool compressed: {false, true}) {
        const auto map = parsed(compressed ? refPack(testMap()) : testMap());
        CHECK(map.warnings.empty());
        CHECK(map.width == 5);
        CHECK(map.height == 4);
        CHECK(map.border == 1);
        REQUIRE(map.boundaries.size() == 1);
        CHECK(map.boundaries[0] == std::array<std::int32_t, 2>{3, 2});
        REQUIRE(map.heights.size() == 20);
        CHECK(map.heights[19] == 38);

        REQUIRE(map.hasTiles);
        CHECK(map.tiles[1] == ((2 << 2) | 1));
        CHECK(map.blends[6] == 1);
        CHECK(map.cliffs[7] == 1);
        REQUIRE(map.textureClasses.size() == 2);
        CHECK(map.textureClasses[1].name == "TestRock");
        CHECK(map.textureClasses[1].width == 2);
        REQUIRE(map.blendTiles.size() == 2);
        CHECK(map.blendTiles[1].tile == (3 << 2));
        CHECK(map.blendTiles[1].horizontal);
        CHECK(map.blendTiles[1].inverted == 1);
        REQUIRE(map.cliffInfos.size() == 2);
        CHECK(map.cliffInfos[1].flip);
        CHECK(map.cliffInfos[1].uv[1] == Approx(-0.03125f));

        REQUIRE(map.objects.size() == 2);
        const auto& tree = map.objects[0];
        CHECK(tree.templateName == "TestTree");
        CHECK(tree.x == 15);
        CHECK(tree.z == 2);
        CHECK(tree.angle == Approx(1.5f));
        REQUIRE(tree.properties.size() == 5);
        CHECK(findDictValue(tree.properties, "objectInitialHealth")->intValue == 100);
        CHECK(findDictValue(tree.properties, "objectScale")->toString() == "1.5");
        CHECK(findDictValue(tree.properties, "objectIndestructible")->toString() == "true");
        CHECK(findDictValue(tree.properties, "objectName")->text == "Oak");
        CHECK(findDictValue(tree.properties, "objectName")->type == DictValue::unicode);

        REQUIRE(map.polygons.size() == 2);
        CHECK(map.polygons[0].water);
        CHECK(map.polygons[0].points.size() == 4);
        CHECK_FALSE(map.polygons[1].water);

        REQUIRE(map.sides.size() == 1);
        CHECK(findDictValue(map.sides[0].player, "playerName")->text == "PlyrRed");
        REQUIRE(map.teams.size() == 1);
        CHECK(findDictValue(map.worldInfo, "weather")->intValue == 1);
        REQUIRE(map.hasLighting);
        CHECK(map.lighting.timeOfDay == 2);
        CHECK(map.lighting.terrain[1].ambient[0] == Approx(0.5f));
        CHECK(map.lighting.objects[1].ambient[0] == Approx(0.25f));
        CHECK(map.lighting.shadowColor == 0x7F000000u);
        CHECK(map.chunkNames.back() == "MapPreview");
        CHECK(groundHeight(map, 0, 0) == Approx(12 * mapHeightScale));  // vertex (1, 1)
        CHECK(groundHeight(map, 5, 0) == Approx(13 * mapHeightScale));
    }
}

TEST_CASE("Damaged maps give warnings or an error, not a crash") {
    const auto bytes = testMap();
    std::string error;
    MapFile map;
    CHECK_FALSE(parseMap("not a map", map, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(parseMap(bytes.substr(0, 40), map, error));

    // Cut anywhere: either an error or a map with warnings.
    for (std::size_t cut = 50; cut < bytes.size(); cut += 37) {
        MapFile cutMap;
        std::string cutError;
        if (parseMap(bytes.substr(0, cut), cutMap, cutError)) {
            CHECK_FALSE(cutMap.heights.empty());
        } else {
            CHECK_FALSE(cutError.empty());
        }
    }

    // A broken chunk is skipped; the chunks after it still load.
    auto damaged = bytes;
    const auto blendMarker = damaged.find(std::string("\x00\x00\xDA\x7A", 4));
    REQUIRE(blendMarker != std::string::npos);
    damaged[blendMarker + 2] = 0;
    MapFile damagedMap;
    REQUIRE(parseMap(damaged, damagedMap, error));
    CHECK_FALSE(damagedMap.hasTiles);
    CHECK_FALSE(damagedMap.warnings.empty());
    CHECK(damagedMap.objects.size() == 2);
}

TEST_CASE("Polygon triangulation") {
    // An L shape, both windings: 6 corners -> 4 triangles covering its area (3 squares).
    std::vector<std::array<float, 2>> outline{{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
    for (int pass = 0; pass < 2; ++pass) {
        const auto indices = triangulatePolygon(outline);
        REQUIRE(indices.size() == 12);
        float area = 0;
        for (std::size_t i = 0; i < indices.size(); i += 3) {
            const auto &a = outline[indices[i]], &b = outline[indices[i + 1]], &c = outline[indices[i + 2]];
            area += std::abs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) / 2;
        }
        CHECK(area == Approx(3));
        std::reverse(outline.begin(), outline.end());
    }
    CHECK(triangulatePolygon({{0, 0}, {1, 0}}).empty());
}

TEST_CASE("Map scene: terrain, water, objects") {
    const auto folder = makeTempFolder("map-scene");
    makeMapGameFolder(folder);
    GameFiles files;
    std::vector<std::string> warnings;
    std::string error;
    REQUIRE(files.mount({folder}, warnings, error));
    const auto map = parsed(testMap());
    ObjectInfo tree;
    tree.name = "TestTree";
    tree.model = "TTree";
    const auto scene = buildMapScene(map, files);
    CHECK(scene.warnings.empty());

    // Atlas: grass (64 px), rock (128 px) and the placeholder, each with a 4 px border.
    CHECK(scene.atlas.width == 2048);
    CHECK(scene.atlas.height == 136);
    REQUIRE(scene.chunks.size() == 1);
    const auto& chunk = scene.chunks[0];
    CHECK(chunk.vertices.size() == 4 * 4 * 3);
    CHECK(chunk.indices.size() == 6 * 4 * 3);

    // Cell 0 is at the map's corner, one cell outside the playable area.
    const auto& first = chunk.vertices[0];
    CHECK(first.position[0] == -10);
    CHECK(first.position[1] == -10);
    CHECK(first.position[2] == 0);
    CHECK(chunk.vertices[2].position[2] == Approx(12 * mapHeightScale));  // corner (1, 1)

    // Cell 1: rock tile 2 -> column 1, bottom row, east half, south half of the 128 px image
    // (placed first, at 4, 4).
    const auto& rock = chunk.vertices[4];
    CHECK(rock.uv[0] * 2048 == Approx(4 + 64 + 32));
    CHECK(rock.uv[1] * 136 == Approx(4 + 64 + 64));  // corner 0 is the piece's bottom left
    CHECK(chunk.vertices[6].uv[1] * 136 == Approx(4 + 64 + 32));

    // Cell 6 (x 1, y 1): rock blended over grass, horizontal inverted: west corners rock.
    const auto* blended = &chunk.vertices[(1 * 4 + 1) * 4];
    CHECK(blended[0].alpha[0] == 1);
    CHECK(blended[1].alpha[0] == 0);
    CHECK(blended[2].alpha[0] == 0);
    CHECK(blended[3].alpha[0] == 1);
    CHECK(blended[0].blendUv[0] != blended[0].uv[0]);

    // Cell 7: cliff stretched corners, relative to the grass slot's bottom left (slot at 140, 4).
    const auto* cliff = &chunk.vertices[(1 * 4 + 2) * 4];
    CHECK(cliff[0].uv[0] * 2048 == Approx(140));
    CHECK(cliff[0].uv[1] * 136 == Approx(4 + 64 - 64));
    CHECK(cliff[1].uv[0] * 2048 == Approx(140 + 32));

    CHECK(scene.playable == std::array<float, 4>{0, 0, 30, 20});

    REQUIRE(scene.water.size() == 1);
    CHECK(scene.water[0].height == 7);
    CHECK(scene.water[0].indices.size() == 6);
    CHECK(scene.waterTexture.width == 2);
    CHECK(scene.waterLook.transparentDiffuse[3] == Approx(0.6f));
    CHECK(scene.waterLook.standingColor[2] == Approx(1));

    const auto findObject = [&](const std::string& name) {
        return name == "TestTree" ? &tree : nullptr;
    };
    REQUIRE(map.objects.size() == 2);
    const auto placedTree = resolveObject(map, map.objects[0], findObject);
    CHECK(placedTree.kind == "model");
    CHECK(placedTree.model == "TTree");
    CHECK(placedTree.color == 0xFF0000);
    CHECK(placedTree.z == Approx(groundHeight(map, 15, 5) + 2));
    const auto placedWaypoint = resolveObject(map, map.objects[1], findObject);
    CHECK(placedWaypoint.kind == "waypoint");
    CHECK(placedWaypoint.color == 0xFFFFFF);
}

TEST_CASE("EditorCore lists and opens maps") {
    const auto folder = makeTempFolder("map-core");
    makeMapGameFolder(folder);
    const auto dbPath = (folder / "store.sqlite").string();
    EditorCore core(dbPath);
    auto reader = makeStorage(dbPath);

    CHECK(core.send("map.open\tmaps/test map/test map.map") == "error:no_game");
    REQUIRE(core.send("game.open\t" + folder.string()).empty());
    core.update();
    REQUIRE(reader.get<StatusRow>(1).state == "ready");
    auto maps = reader.get_all<MapListRow>(sqlite_orm::order_by(&MapListRow::path));
    REQUIRE(maps.size() == 2);
    CHECK(maps[0].name == "Loose");
    CHECK(maps[0].source.find("Loose.map") != std::string::npos);
    CHECK(maps[1].name == "Test Map");
    CHECK(maps[1].source == "MapsZH.big");
    CHECK(reader.get<MapRow>(1).state == "idle");

    CHECK(core.send("map.open") == "error:missing_argument");
    REQUIRE(core.send("map.open\t" + maps[1].path).empty());
    CHECK(reader.get<MapRow>(1).state == "loading");
    core.update();
    auto row = reader.get<MapRow>(1);
    REQUIRE(row.state == "ready");
    CHECK(row.name == "Test Map");
    CHECK(row.width == 5);
    CHECK(row.heights.size() == 20);
    CHECK(row.lighting.size() == 18 * 4);
    CHECK(row.objectCount == 2);
    CHECK(row.modelCount == 1);
    CHECK(row.modelsLoaded == 0);
    CHECK(row.waterA == Approx(0.6));
    CHECK(reader.count<MapTerrainRow>() == 1);
    CHECK(reader.count<MapWaterRow>() == 1);
    CHECK(reader.count<MapTextureRow>() == 2);
    CHECK(reader.count<MapPropertyRow>() == 7);
    CHECK(reader.count<MapWarningRow>() == 0);
    const auto objects = reader.get_all<MapObjectRow>(sqlite_orm::order_by(&MapObjectRow::id));
    REQUIRE(objects.size() == 2);
    CHECK(objects[0].modelId == 1);
    CHECK(objects[1].modelId == 0);
    CHECK(reader.get<MapModelRow>(1).state == "pending");

    // An edit runs in the next update, before the models still to load.
    REQUIRE(core.send("map.move\t1\t16\t6").empty());
    core.update();
    CHECK(reader.get<MapObjectRow>(1).x == 16);
    CHECK(reader.get<MapModelRow>(1).state == "pending");

    // Models come in the next updates.
    REQUIRE(core.hasPendingWork());
    while (core.hasPendingWork()) {
        core.update();
    }
    const auto model = reader.get<MapModelRow>(1);
    CHECK(model.state == "ready");
    CHECK(model.maxZ == Approx(10));  // the trunk hangs on bone TOP, 10 units up
    CHECK(reader.get<MapRow>(1).modelsLoaded == 1);
    CHECK(reader.get<MapRow>(1).modelRevision > row.modelRevision);
    CHECK(reader.count<MapModelPartRow>() == 1);
    CHECK(reader.count<MapModelTextureRow>() == 1);

    // The loose, uncompressed copy, and a file path.
    REQUIRE(core.send("map.open\t" + maps[0].path).empty());
    core.update();
    CHECK(reader.get<MapRow>(1).state == "ready");
    REQUIRE(core.send("map.open\t" + (folder / "Maps" / "Loose" / "Loose.map").string()).empty());
    core.update();
    CHECK(reader.get<MapRow>(1).state == "ready");
    CHECK(reader.get<MapRow>(1).name == "Loose");
    REQUIRE(core.send("map.open\tmaps/none/none.map").empty());
    core.update();
    CHECK(reader.get<MapRow>(1).state == "error");
    CHECK(reader.count<MapObjectRow>() == 0);
}

TEST_CASE("Terrain.ini copies of later folders fill in missing names") {
    const auto mod = makeTempFolder("terrain-mod");
    const auto base = makeTempFolder("terrain-base");
    writeBig(mod / "INIZH.big", {{"Data\\INI\\Terrain.ini", "Terrain TestGrass\n  Texture = TGrass.tga\nEnd\n"}});
    writeBig(base / "INI.big",
             {{"Data\\INI\\Terrain.ini",
               "Terrain TestGrass\n  Texture = Old.tga\nEnd\n"
               "Terrain TestRock\n  Texture = TRock.tga\nEnd\n"}});
    writeBig(base / "Terrain.big",
             {{"Art\\Terrain\\TGrass.tga", tga(64, 64, pixels(64 * 64, 0, 200, 0))},
              {"Art\\Terrain\\TRock.tga", tga(128, 128, pixels(128 * 128, 120, 120, 120))}});
    GameFiles files;
    std::vector<std::string> warnings;
    std::string error;
    REQUIRE(files.mount({mod, base}, warnings, error));
    CHECK(files.readEach("data/ini/terrain.ini", [](const std::string&) {}) == 2);
    const auto scene = buildMapScene(parsed(testMap()), files);
    CHECK(scene.warnings.empty());
    // Grass came from the first copy: its texture is green, not the placeholder.
    const auto& uv = scene.chunks[0].vertices[0].uv;
    const auto x = std::size_t(uv[0] * float(scene.atlas.width)),
               y = std::size_t(uv[1] * float(scene.atlas.height)) - 1;
    CHECK(int(scene.atlas.rgba[(y * scene.atlas.width + x) * 4 + 1]) == 200);
}
