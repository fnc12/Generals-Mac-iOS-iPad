#include "compression.h"
#include "editor_core.h"
#include "map_document.h"
#include "map_file.h"
#include "test_big_writer.h"
#include "test_map_writer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>

using namespace end_editor;
using namespace end_editor_tests;
using Catch::Approx;

namespace {
    MapFile parsed(const std::string& bytes) {
        MapFile map;
        std::string error;
        REQUIRE(parseMap(bytes, map, error));
        CHECK(error.empty());
        return map;
    }

    std::string readFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    // The test map with a second, older Object chunk (version 2) and one the engine drops (far
    // below the ground), to check that both stay as they are.
    std::string mapWithOddObjects() {
        ChunkWriter w;
        w.open("HeightMapData", 4);
        w.body.u32(3).u32(3).u32(0).u32(1).u32(2).u32(2).u32(9);
        w.body.data.append(9, char(16));
        w.close();
        w.open("SidesList", 3);
        w.body.u32(1);
        w.dict({{"playerName", 3, 0, 0, ""}, {"playerDisplayName", 4, 0, 0, "Neutral"}});
        w.body.u32(0).u32(1);
        w.dict({{"teamName", 3, 0, 0, "team"}, {"teamOwner", 3, 0, 0, ""}});
        w.close();
        w.open("ObjectsList", 3);
        w.open("Object", 2);
        w.body.floats({5, 5, 9, 0}).u32(0);
        w.ascii("OldRock");
        w.dict({{"originalOwner", 3, 0, 0, "team"}});
        w.close();
        w.open("Object", 3);
        w.body.floats({6, 6, -5000, 0}).u32(0);
        w.ascii("Buried");
        w.dict({});
        w.close();
        w.close();
        return w.file();
    }
}

TEST_CASE("MapDocument saves an unchanged map byte for byte") {
    const auto bytes = testMap();
    MapDocument document(parsed(bytes));
    CHECK(document.save() == bytes);
    CHECK_FALSE(document.edited());

    // A compressed map is written unpacked: the same bytes as its content.
    MapDocument packed(parsed(refPack(bytes)));
    CHECK(packed.save() == bytes);

    const auto odd = mapWithOddObjects();
    MapDocument oddDocument(parsed(odd));
    CHECK(oddDocument.save() == odd);
    // The dropped object has no id and is not shown, but stays in the file.
    REQUIRE(oddDocument.objects().size() == 2);
    CHECK(oddDocument.objects()[0].id == 1);
    CHECK(oddDocument.objects()[1].id == 0);
}

TEST_CASE("MapDocument edits: move, turn, remove, place, undo, redo") {
    MapDocument document(parsed(testMap()));
    REQUIRE(document.find(1));
    CHECK(document.find(1)->object.templateName == "TestTree");
    CHECK(document.find(2)->object.templateName == "*Waypoints/Waypoint");
    CHECK_FALSE(document.find(3));

    CHECK(document.move(1, 12, 7));
    CHECK(document.rotate(1, 0.25f));
    CHECK(document.remove(2));
    CHECK_FALSE(document.find(2));
    CHECK_FALSE(document.move(2, 0, 0));
    CHECK_FALSE(document.remove(99));
    const auto placed = document.place("TestRock", 20, 10, 7.0f, "teamPlyrRed");  // 7 rad -> 7 - 2 pi
    CHECK(placed == 3);
    CHECK(document.lastChangedId() == 3);
    CHECK(document.undoCount() == 4);
    CHECK(document.edited());

    // Saved and read again.
    auto saved = parsed(document.save());
    REQUIRE(saved.objects.size() == 2);
    const auto& tree = saved.objects[0];
    CHECK(tree.templateName == "TestTree");
    CHECK(tree.x == 12);
    CHECK(tree.y == 7);
    CHECK(tree.z == 2);  // height above the ground stays
    CHECK(tree.angle == Approx(0.25f));
    CHECK(tree.properties.size() == 5);  // its properties are untouched
    const auto& rock = saved.objects[1];
    CHECK(rock.templateName == "TestRock");
    CHECK(rock.x == 20);
    CHECK(rock.y == 10);
    CHECK(rock.z == 0);
    CHECK(rock.angle == Approx(7.0f - 2 * 3.14159265f));
    REQUIRE(findDictValue(rock.properties, "originalOwner"));
    CHECK(findDictValue(rock.properties, "originalOwner")->text() == "teamPlyrRed");
    REQUIRE(findDictValue(rock.properties, "uniqueID"));
    CHECK(findDictValue(rock.properties, "uniqueID")->text() == "TestRock 0");
    REQUIRE(findDictValue(rock.properties, "objectInitialHealth"));
    CHECK(findDictValue(rock.properties, "objectInitialHealth")->data == DictValue::Data(std::int32_t(100)));
    CHECK(findDictValue(rock.properties, "objectPowered")->data == DictValue::Data(true));
    // Everything but ObjectsList and the names it added is as it was.
    const auto original = parsed(testMap());
    CHECK(saved.chunkNames == original.chunkNames);
    CHECK(saved.heights == original.heights);
    CHECK(saved.teams.size() == original.teams.size());
    CHECK(saved.polygons.size() == original.polygons.size());
    for (std::size_t i = 0; i < saved.chunks.size(); ++i) {
        if (saved.chunks[i].name == "ObjectsList") {
            continue;
        }
        const auto &a = saved.chunks[i], &b = original.chunks[i];
        CHECK(saved.data.substr(a.header + 4, a.end - a.header - 4) ==
              original.data.substr(b.header + 4, b.end - b.header - 4));
    }

    // Undo all four, then redo two.
    CHECK(document.undo());
    CHECK_FALSE(document.find(3));
    CHECK(document.undo());
    CHECK(document.find(2));
    CHECK(document.undo());
    CHECK(document.find(1)->object.angle == Approx(1.5f));
    CHECK(document.undo());
    CHECK(document.find(1)->object.x == 15);
    CHECK_FALSE(document.undo());
    CHECK_FALSE(document.edited());
    CHECK(document.redoCount() == 4);
    CHECK(document.redo());
    CHECK(document.redo());
    CHECK(document.find(1)->object.x == 12);
    CHECK(document.find(1)->object.angle == Approx(0.25f));
    CHECK(document.find(2));
    // A new edit drops what could still be redone.
    CHECK(document.remove(2));
    CHECK(document.redoCount() == 0);
    document.markSaved();
    CHECK_FALSE(document.edited());
    CHECK(document.undo());
    CHECK(document.edited());
    CHECK(document.move(1, 1, 1));  // a different edit at the same depth
    CHECK(document.edited());

    // With nothing left, the undone placement is not written: the saved map has the tree only.
    MapDocument fresh(parsed(testMap()));
    fresh.place("TestRock", 1, 1, 0, "teamPlyrRed");
    fresh.undo();
    CHECK(parsed(fresh.save()).objects.size() == 2);
}

TEST_CASE("MapDocument keeps unread objects and adds ObjectsList when there is none") {
    MapDocument document(parsed(mapWithOddObjects()));
    CHECK(document.move(1, 2, 3));
    const auto saved = parsed(document.save());
    REQUIRE(saved.objects.size() == 1);
    CHECK(saved.objects[0].x == 2);
    CHECK(saved.objects[0].z == 0);  // version 2: the engine ignores the stored height
    CHECK(saved.objectChunks.size() == 2);
    CHECK(saved.objectChunks[0].version == 2);

    // No ObjectsList at all: placing an object adds one (and its names) at the end.
    ChunkWriter w;
    w.open("HeightMapData", 4);
    w.body.u32(3).u32(3).u32(0).u32(1).u32(2).u32(2).u32(9);
    w.body.data.append(9, char(0));
    w.close();
    MapDocument bare(parsed(w.file()));
    CHECK(bare.save() == w.file());
    bare.place("TestRock", 5, 5, 0, "team");
    const auto withObjects = parsed(bare.save());
    REQUIRE(withObjects.objects.size() == 1);
    CHECK(withObjects.chunkNames.back() == "ObjectsList");
    CHECK(withObjects.objects[0].templateName == "TestRock");
}

TEST_CASE("Map players: the SidesList players with a default team") {
    const auto players = mapPlayers(parsed(testMap()));
    REQUIRE(players.size() == 1);
    CHECK(players[0].name == "PlyrRed");
    CHECK(players[0].team == "teamPlyrRed");
    CHECK(players[0].color == 0xFF0000);
    CHECK_FALSE(players[0].neutral);
    const auto neutral = mapPlayers(parsed(mapWithOddObjects()));
    REQUIRE(neutral.size() == 1);
    CHECK(neutral[0].neutral);
    CHECK(neutral[0].team == "team");
    CHECK(neutral[0].displayName == "Neutral");
}

TEST_CASE("Map players: a multiplayer map's start positions follow the SidesList players") {
    // One start position: a mission, its own players only.
    CHECK(mapStartPositions(parsed(testMap())) == 1);
    CHECK(mapPlayers(parsed(testMap())).size() == 1);
    CHECK(mapTeams(parsed(testMap())) == std::vector<std::string>{"teamPlyrRed"});

    const auto map = parsed(testMap(3));
    CHECK(mapStartPositions(map) == 3);
    const auto players = mapPlayers(map);
    REQUIRE(players.size() == 4);
    CHECK(players[0].team == "teamPlyrRed");
    CHECK(players[0].start == 0);
    for (int start = 1; start <= 3; ++start) {
        const auto& player = players[std::size_t(start)];
        CHECK(player.name == "Start" + std::to_string(start));
        CHECK(player.displayName == "Start " + std::to_string(start));
        CHECK(player.team == "teamStart" + std::to_string(start));
        CHECK(player.start == start);
        CHECK(player.color == startPositionColor(start));
        CHECK_FALSE(player.neutral);
    }
    CHECK(mapTeams(map) == std::vector<std::string>{"teamPlyrRed", "teamStart1", "teamStart2", "teamStart3"});

    // Counted like the game's map cache: from Player_1_Start up to the first number missing, at
    // most 8, only waypoints.
    const auto waypoint = [](const std::string& name, const std::string& templateName = waypointTemplate) {
        MapObject object;
        object.templateName = templateName;
        object.properties.push_back(DictValue{"waypointName", AsciiText{name}});
        return object;
    };
    MapFile gaps;
    for (const auto* name: {"Player_2_Start", "Player_1_Start", "Player_4_Start", "player_3_start"}) {
        gaps.objects.push_back(waypoint(name));
    }
    gaps.objects.push_back(waypoint("Player_3_Start", "TestRock"));
    CHECK(mapStartPositions(gaps) == 2);
    MapFile many;
    for (int n = 1; n <= 9; ++n) {
        many.objects.push_back(waypoint("Player_" + std::to_string(n) + "_Start"));
    }
    many.objects.push_back(waypoint("Player_10_Start"));
    CHECK(mapStartPositions(many) == 8);
    CHECK(mapPlayers(many).size() == 8);
    CHECK(mapStartPositions(MapFile{}) == 0);

    // A SidesList that already has a start position's team keeps it once.
    auto withStart = parsed(testMap(2));
    withStart.teams.push_back(Dict{DictValue{"teamName", AsciiText{"teamStart2"}}});
    CHECK(mapTeams(withStart) == std::vector<std::string>{"teamPlyrRed", "teamStart2", "teamStart1"});
}

TEST_CASE("Player colors: Multiplayer.ini's MultiplayerColor entries in file order") {
    const auto colors = parsePlayerColors("OnlineChatColors\n  Default = R:255 G:255 B:255\nEnd\n"
                                          "MultiplayerSettings\n  ShowRandomColor = Yes\nEnd\n"
                                          "MultiplayerColor ColorGold\n  RGBColor = R:221 G:226 B:13\n"
                                          "  TooltipName = Color:Gold\nEnd\n"
                                          "; MultiplayerColor ColorNot\n"
                                          "multiplayercolor ColorBlue ; a comment\n  RGBColor = R:67 G:104 B:254\nEND\n"
                                          "MultiplayerColor ColorBare\nEnd\n");
    REQUIRE(colors.size() == 3);
    CHECK(colors[0].name == "ColorGold");
    CHECK(colors[0].rgb == 0xDDE20D);
    CHECK(colors[1].name == "ColorBlue");
    CHECK(colors[1].rgb == 0x4368FE);
    CHECK(colors[2].name == "ColorBare");
    CHECK(colors[2].rgb == 0xFFFFFF);
}

TEST_CASE("EditorCore edits and saves the open map") {
    const auto folder = makeTempFolder("map-edit");
    writeBig(folder / "MapsZH.big", {{"Maps\\Test Map\\Test Map.map", refPack(testMap())}});
    writeBig(folder / "INIZH.big",
             {{"Data\\INI\\Object\\Rocks.ini",
               "Object TestRock\n  EditorSorting = MISC_NATURAL\nEnd\n"
               "Object TestTree\nEnd\n"}});
    const auto userMaps = makeTempFolder("map-edit-user") / "Maps";
    std::filesystem::create_directories(userMaps);
    const auto dbPath = (folder / "store.sqlite").string();
    EditorCore core(dbPath);
    auto reader = makeStorage(dbPath);
    const auto run = [&] {
        while (core.hasPendingWork()) {
            core.update();
        }
    };

    CHECK(core.send("map.place\tTestRock\t1\t1\t0\tteam") == "error:no_map");
    REQUIRE(core.send("maps.user\t" + userMaps.string()).empty());
    REQUIRE(core.send("game.open\t" + folder.string()).empty());
    run();
    REQUIRE(core.send("map.open\tmaps/test map/test map.map").empty());
    run();
    auto row = reader.get<MapRow>(1);
    REQUIRE(row.state == "ready");
    CHECK(row.undoCount == 0);
    CHECK_FALSE(row.edited);
    const auto players = reader.get_all<MapPlayerRow>();
    REQUIRE(players.size() == 1);
    CHECK(players[0].team == "teamPlyrRed");

    CHECK(core.send("map.place\tNoSuchThing\t1\t1\t0\tteam") == "error:unknown_template");
    CHECK(core.send("map.place\tTestRock\tx\t1\t0\tteam") == "error:bad_argument");
    CHECK(core.send("map.move\t1\t2") == "error:missing_argument");
    REQUIRE(core.send("map.place\tTestRock\t25\t15\t0.5\tteamPlyrRed").empty());
    REQUIRE(core.send("map.move\t1\t11\t12").empty());
    REQUIRE(core.send("map.rotate\t1\t-1").empty());
    REQUIRE(core.send("map.delete\t2").empty());
    core.update();  // all edits in one update
    CHECK_FALSE(core.hasPendingWork());
    row = reader.get<MapRow>(1);
    CHECK(row.undoCount == 4);
    CHECK(row.edited);
    CHECK(row.lastObject == 2);
    CHECK(row.objectCount == 2);
    CHECK(row.objectRevision > row.revision);
    auto objects = reader.get_all<MapObjectRow>(sqlite_orm::order_by(&MapObjectRow::id));
    REQUIRE(objects.size() == 2);
    CHECK(objects[0].x == 11);
    CHECK(objects[0].y == 12);
    CHECK(objects[0].angle == Approx(-1));
    CHECK(objects[0].z > 2);  // the ground under it plus its offset
    CHECK(objects[1].id == 3);
    CHECK(objects[1].templateName == "TestRock");
    CHECK(objects[1].kind == "marker");  // no model in this game folder
    CHECK(objects[1].owner == "teamPlyrRed");
    CHECK(objects[1].color == 0xFF0000);
    using namespace sqlite_orm;
    CHECK(reader.count<MapPropertyRow>(where(c(&MapPropertyRow::objectId) == 3)) == 9);
    CHECK(reader.count<MapPropertyRow>(where(c(&MapPropertyRow::objectId) == 2)) == 0);

    REQUIRE(core.send("map.undo").empty());
    core.update();
    CHECK(reader.count<MapObjectRow>() == 3);
    CHECK(reader.get<MapRow>(1).lastObject == 2);
    CHECK(reader.get<MapRow>(1).redoCount == 1);
    REQUIRE(core.send("map.redo").empty());
    core.update();
    CHECK(reader.count<MapObjectRow>() == 2);

    // Saved into the user maps folder: it shows in the map list and opens with the edits.
    const auto saved = userMaps / "Edited" / "Edited.map";
    const auto listRevision = reader.get<StatusRow>(1).mapListRevision;
    const auto statusRevision = reader.get<StatusRow>(1).revision;
    REQUIRE(core.send("map.save\t" + saved.string()).empty());
    run();
    row = reader.get<MapRow>(1);
    CHECK(row.editMessage == "Saved " + saved.string());
    CHECK(row.savedPath == saved.string());
    CHECK_FALSE(row.edited);
    REQUIRE(std::filesystem::exists(saved));
    CHECK(reader.get<StatusRow>(1).mapListRevision > listRevision);
    CHECK(reader.get<StatusRow>(1).revision == statusRevision);
    const auto maps = reader.get_all<MapListRow>(order_by(&MapListRow::name));
    REQUIRE(maps.size() == 2);
    CHECK(maps[0].name == "Edited");
    CHECK(maps[0].source == "User maps");
    CHECK(maps[0].path == saved.string());
    const auto file = parsed(readFile(saved));
    REQUIRE(file.objects.size() == 2);
    CHECK(file.objects[1].templateName == "TestRock");

    REQUIRE(core.send("map.open\t" + maps[0].path).empty());
    run();
    CHECK(reader.get<MapRow>(1).state == "ready");
    CHECK(reader.count<MapObjectRow>() == 2);

    // Never into the game folder.
    REQUIRE(core.send("map.save\t" + (folder / "Maps" / "x" / "x.map").string()).empty());
    run();
    CHECK(reader.get<MapRow>(1).editMessage.rfind("error: not saved", 0) == 0);
    CHECK_FALSE(std::filesystem::exists(folder / "Maps" / "x" / "x.map"));
}

TEST_CASE("MapDocument groups, heights of old objects, properties and existing waypoint links") {
    // A version 2 object (the engine ignores its stored height) and a WaypointsList with one link.
    ChunkWriter w;
    w.open("HeightMapData", 4);
    w.body.u32(3).u32(3).u32(0).u32(1).u32(2).u32(2).u32(9);
    w.body.data.append(9, char(0));
    w.close();
    w.open("ObjectsList", 3);
    w.open("Object", 2);
    w.body.floats({5, 5, 9, 0}).u32(0);
    w.ascii("OldRock");
    w.dict({{"originalOwner", 3, 0, 0, "team"}});
    w.close();
    for (int i = 1; i <= 2; ++i) {
        w.open("Object", 3);
        w.body.floats({float(i * 5), 10, 0, 0}).u32(0);
        w.ascii("*Waypoints/Waypoint");
        w.dict({{"waypointID", 1, i * 10}, {"waypointName", 3, 0, 0, "W" + std::to_string(i)}});
        w.close();
    }
    w.close();
    w.open("WaypointsList", 1);
    w.body.u32(1).u32(10).u32(20);
    w.close();
    w.open("Trailer", 7);
    w.body.u32(99);
    w.close();
    const auto bytes = w.file();
    MapDocument document(parsed(bytes));
    CHECK(document.save() == bytes);
    REQUIRE(document.links().size() == 1);
    CHECK(document.waypointObject(20) == 3);

    // One group, one undo step.
    document.begin();
    CHECK(document.move(1, 1, 1));
    CHECK(document.setHeight(1, 12));
    const DictValue name{"objectName", AsciiText{"Boulder"}};
    CHECK(document.setProperty(1, name));
    document.commit();
    CHECK(document.undoCount() == 1);
    CHECK(document.lastChanged() == std::vector<std::int64_t>{1});
    auto saved = parsed(document.save());
    REQUIRE(saved.objects.size() == 3);
    CHECK(saved.objectChunks[0].version == 3);  // written again: version 2 has no height the engine reads
    CHECK(saved.objects[0].z == 12);
    CHECK(saved.objects[0].x == 1);
    CHECK(findDictValue(saved.objects[0].properties, "objectName")->text() == "Boulder");
    CHECK(findDictValue(saved.objects[0].properties, "originalOwner")->text() == "team");
    CHECK(document.undo());
    CHECK(document.save() == bytes);

    // Links: rewritten in place (same chunk version), the chunk after it stays.
    CHECK(document.toggleLink(3, 2));
    saved = parsed(document.save());
    CHECK(saved.waypointLinks == std::vector<std::array<std::int32_t, 2>>{{10, 20}, {20, 10}});
    CHECK(saved.chunkNames == parsed(bytes).chunkNames);
    CHECK(saved.chunks.back().version == 7);
    CHECK(document.remove(2));  // its links go with it
    CHECK(document.links().empty());
    CHECK(document.undo());
    CHECK(document.links().size() == 2);
    CHECK(document.undo());
    CHECK(document.save() == bytes);
    CHECK_FALSE(document.toggleLink(1, 2));  // not a waypoint

    // A link to a waypoint the map no longer has stays dead: new waypoints get higher ids.
    CHECK(document.remove(3));
    const auto fresh = document.placeWaypoint(1, 1);
    CHECK(document.waypointId(fresh) == 21);
}

TEST_CASE("Start positions: waypoint and team names, and one color each") {
    const auto waypoint = [](const std::string& name, const std::string& templateName = waypointTemplate) {
        MapObject object;
        object.templateName = templateName;
        object.properties.push_back(DictValue{"waypointName", AsciiText{name}});
        return object;
    };
    for (int n = 1; n <= 8; ++n) {
        CHECK(startPositionOf(waypoint("Player_" + std::to_string(n) + "_Start")) == n);
        CHECK(startPositionOfTeam("teamStart" + std::to_string(n)) == n);
    }
    for (const auto* name: {"Player_0_Start",
                            "Player_9_Start",
                            "Player_10_Start",
                            "player_1_start",
                            "Player_1_Star",
                            "Player_1_Start2",
                            "Start 1",
                            ""}) {
        CHECK(startPositionOf(waypoint(name)) == 0);
    }
    CHECK(startPositionOf(waypoint("Player_1_Start", "TestRock")) == 0);
    CHECK(startPositionOf(MapObject{}) == 0);
    for (const auto* team: {"teamStart0", "teamStart9", "teamStart10", "teamstart1", "teamStart", "teamPlyrRed", ""}) {
        CHECK(startPositionOfTeam(team) == 0);
    }
    // Eight different colors, none white (no owner) or near the waypoint markers' gold.
    std::vector<std::uint32_t> colors;
    for (int n = 1; n <= 8; ++n) {
        const auto color = startPositionColor(n);
        CHECK(color != 0xFFFFFF);
        CHECK(std::find(colors.begin(), colors.end(), color) == colors.end());
        colors.push_back(color);
    }
    CHECK(startPositionColor(0) == 0xFFFFFF);
    CHECK(startPositionColor(9) == 0xFFFFFF);
}
