// Whole user flows as the macOS client drives them: input events and inspector edits become
// commands through the C ABI, and every check reads the SQLite store with a second connection,
// the way the client does. No client logic is needed for any of these rules.
#include "compression.h"
#include "end_editor/editor_core_c.h"
#include "map_file.h"
#include "map_preview.h"
#include "storage.h"
#include "test_big_writer.h"
#include "test_map_writer.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <thread>

#include <signal.h>

using namespace end_editor;
using namespace end_editor_tests;
using Catch::Approx;

namespace {
    std::string readFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    MapFile parsed(const std::string& bytes) {
        MapFile map;
        std::string error;
        REQUIRE(parseMap(bytes, map, error));
        return map;
    }

    // A game folder with the test map, three object templates and a Roads.ini, and the core
    // opened on it through the C ABI.
    class Flow {
      public:
        explicit Flow(const std::string& name) {
            this->folder = makeTempFolder(name);
            this->saved = makeTempFolder(name + "-saved");  // never inside the game folder
            writeBig(this->folder / "MapsZH.big", {{"Maps\\Test Map\\Test Map.map", refPack(testMap())}});
            writeBig(
                this->folder / "INIZH.big",
                {{"Data\\INI\\Object\\Rocks.ini", "Object TestRock\nEnd\nObject TestTree\nEnd\nObject TestTank\nEnd\n"},
                 {"Data\\INI\\Roads.ini",
                  "Road TwoLane\n  Texture = TRTwoLane.tga\n  RoadWidth = 35\nEnd\n"
                  "Bridge WoodBridge\n  BridgeScale = 1.0\nEnd\n"},
                 {"Data\\INI\\PlayerTemplate.ini",
                  "PlayerTemplate FactionCivilian\n  Side = Civilian\n  PlayableSide = No\nEnd\n"
                  "PlayerTemplate FactionAmerica\n  Side = America\n  PlayableSide = Yes\nEnd\n"
                  "; a comment\nPlayerTemplate FactionChina\n  Side = China\n  PlayableSide = Yes\nEnd\n"}});
            this->dbPath = (this->folder / "store.sqlite").string();
            REQUIRE(end_editor_initialize(this->dbPath.c_str()));
            this->reader.emplace(makeStorage(this->dbPath));
            this->send("game.open\t" + this->folder.string());
            this->send("map.open\tmaps/test map/test map.map");
            REQUIRE(this->reader->get<MapRow>(1).state == "ready");
        }
        ~Flow() {
            end_editor_shutdown();
        }

        // Sends a command the client would send and runs the work it queued.
        void send(const std::string& command) {
            const std::string result = end_editor_send(command.c_str());
            INFO(command);
            REQUIRE(result.empty());
            this->run();
        }
        std::string refused(const std::string& command) {
            const std::string result = end_editor_send(command.c_str());
            this->run();
            return result;
        }
        void run() {
            while (end_editor_pending_work() > 0) {
                end_editor_update();
            }
        }

        MapEditorRow editor() {
            return this->reader->get<MapEditorRow>(1);
        }
        std::vector<std::int64_t> selection() {
            std::vector<std::int64_t> ids;
            for (const auto& row: this->reader->iterate<MapSelectionRow>(sqlite_orm::order_by(&MapSelectionRow::id))) {
                ids.push_back(row.objectId);
            }
            return ids;
        }
        std::vector<MapObjectRow> objects() {
            return this->reader->get_all<MapObjectRow>(sqlite_orm::order_by(&MapObjectRow::id));
        }
        std::optional<MapObjectRow> object(std::int64_t id) {
            if (auto row = this->reader->get_pointer<MapObjectRow>(id)) {
                return *row;
            }
            return std::nullopt;
        }
        std::string property(std::int64_t id, const std::string& key) {
            using namespace sqlite_orm;
            for (const auto& row: this->reader->iterate<MapPropertyRow>(
                     where(c(&MapPropertyRow::objectId) == id && c(&MapPropertyRow::key) == key),
                     limit(1))) {
                return row.type + ":" + row.value;
            }
            return "<none>";
        }
        std::vector<MapLinkRow> links() {
            return this->reader->get_all<MapLinkRow>(sqlite_orm::order_by(&MapLinkRow::id));
        }
        MapFile save(const std::string& name) {
            const auto path = this->saved / (name + ".map");
            this->send("map.save\t" + path.string());
            REQUIRE(this->reader->get<MapRow>(1).savedPath == path.string());
            return parsed(readFile(path));
        }

        std::filesystem::path folder, saved;
        std::string dbPath;
        std::optional<decltype(makeStorage(""))> reader;
    };

    using Ids = std::vector<std::int64_t>;
}

TEST_CASE("Flow: a right click with no object chosen asks for one and places it where clicked") {
    Flow flow("flow-pending");
    REQUIRE(flow.editor().templateName.empty());
    const auto before = flow.objects().size();

    flow.send("map.click_right\t12\t8\t");
    auto editor = flow.editor();
    CHECK(editor.picker);  // the client opens the picker because the store says so
    CHECK(editor.pending);
    CHECK(editor.pendingX == 12);
    CHECK(editor.pendingY == 8);
    CHECK(editor.noteKind == "info");
    CHECK_FALSE(editor.note.empty());
    CHECK(flow.objects().size() == before);  // nothing placed yet

    // Another tick, another read: the note is still there (the client does not own it).
    flow.run();
    CHECK(flow.editor().note == editor.note);

    flow.send("placement.template\tTestRock");
    editor = flow.editor();
    CHECK_FALSE(editor.picker);
    CHECK_FALSE(editor.pending);
    CHECK(editor.templateName == "TestRock");
    CHECK(editor.note.empty());  // placing ended the hint
    const auto objects = flow.objects();
    REQUIRE(objects.size() == before + 1);
    const auto& placed = objects.back();
    CHECK(placed.templateName == "TestRock");
    CHECK(placed.x == 12);
    CHECK(placed.y == 8);
    CHECK(placed.owner == "teamPlyrRed");  // the map's first player
    CHECK(flow.selection() == Ids{placed.id});
    CHECK(flow.reader->get<MapRow>(1).undoCount == 1);

    // Dismissing the picker drops the waiting right click.
    flow.send("placement.pick");
    CHECK(flow.editor().picker);
    flow.send("placement.cancel");
    CHECK_FALSE(flow.editor().picker);
    // Now an object is chosen: a right click places at once, with the angle and owner set.
    flow.send("placement.angle\t90");
    flow.send("map.click_right\t20\t4\t3");
    const auto second = flow.objects().back();
    CHECK(second.id == placed.id + 1);
    CHECK(second.x == 20);
    CHECK(second.angle == Approx(3.14159265 / 2));
    CHECK_FALSE(flow.editor().picker);
    // Off the map: told so, nothing placed.
    flow.send("map.click_right\t\t\t");
    CHECK(flow.editor().noteKind == "error");
    CHECK(flow.objects().size() == before + 2);
}

TEST_CASE("Flow: notes last until the next note or map change") {
    Flow flow("flow-notes");
    CHECK(flow.refused("placement.owner\t") == "error:missing_argument");
    CHECK(flow.editor().noteKind == "error");
    flow.send("placement.owner\tteamNobody");
    auto editor = flow.editor();
    CHECK(editor.noteKind == "error");
    CHECK(editor.team == "teamPlyrRed");
    const auto serial = editor.noteSerial;
    // Selecting, panning and ticking keep it.
    flow.send("map.press\t15\t5\t\t1");
    flow.send("map.release\t");
    flow.send("map.press\t0\t0\t\t");
    flow.send("map.drag\t3\t3");
    CHECK(flow.editor().gesture == "pan");
    flow.send("map.release\t");
    flow.run();
    CHECK(flow.editor().noteSerial == serial);
    CHECK(flow.editor().noteKind == "error");
    // The next edit ends it.
    flow.send("edit.rotate\t15");
    CHECK(flow.editor().note.empty());
    CHECK(flow.refused("map.click_right\tx\t1\t") == "error:bad_argument");
    CHECK(flow.editor().noteKind == "error");
    CHECK(flow.refused("placement.template\tNoSuchThing") == "error:unknown_template");
    CHECK(flow.editor().templateName.empty());
    flow.send("note.clear");
    CHECK(flow.editor().note.empty());
}

TEST_CASE("Flow: clicks, shift clicks and a rubber band select; a drag on the selection moves it") {
    Flow flow("flow-select");
    flow.send("placement.template\tTestRock");
    flow.send("map.click_right\t20\t10\t");  // id 3, selected
    REQUIRE(flow.selection() == Ids{3});

    // A click on the tree selects only it; a click on nothing clears.
    flow.send("map.press\t15\t5\t\t1");
    CHECK(flow.editor().gesture == "press_pan");
    flow.send("map.release\t");
    CHECK(flow.selection() == Ids{1});
    flow.send("map.press\t1\t1\t\t");
    flow.send("map.release\t");
    CHECK(flow.selection().empty());

    // Shift clicks add and remove.
    flow.send("map.press\t15\t5\tshift\t1");
    flow.send("map.release\t");
    flow.send("map.press\t20\t10\tshift\t3");
    flow.send("map.release\t");
    CHECK(flow.selection() == Ids{1, 3});
    flow.send("map.press\t15\t5\tshift\t1");
    flow.send("map.release\t");
    CHECK(flow.selection() == Ids{3});

    // A shift drag is a rubber band: what the client found inside it is added.
    flow.send("map.press\t0\t0\tshift\t");
    flow.send("map.drag\t30\t30");
    CHECK(flow.editor().gesture == "band");
    flow.send("map.release\t1,2");
    CHECK(flow.selection() == Ids{3, 1, 2});
    flow.send("selection.set\t1,3");
    CHECK(flow.selection() == Ids{1, 3});

    // ⌥ or ⌘ drags turn the camera: nothing else changes.
    flow.send("map.press\t15\t5\talt\t1");
    flow.send("map.drag\t16\t5");
    CHECK(flow.editor().gesture == "orbit");
    flow.send("map.release\t");
    CHECK(flow.selection() == Ids{1, 3});

    // A press on a selected object (also behind another one) and a drag move the whole selection.
    const auto tree = *flow.object(1), rock = *flow.object(3);
    flow.send("map.press\t20\t10\t\t2,3");
    CHECK(flow.editor().gesture == "press_move");
    flow.send("map.drag\t22\t11");
    flow.send("map.drag\t25\t12");
    auto editor = flow.editor();
    CHECK(editor.gesture == "move");
    CHECK(editor.dragX == 5);  // the client draws the selection this far
    CHECK(editor.dragY == 2);
    CHECK(flow.object(1)->x == tree.x);  // not moved before the release
    flow.send("map.release\t");
    CHECK(flow.editor().gesture.empty());
    CHECK(flow.object(1)->x == tree.x + 5);
    CHECK(flow.object(1)->y == tree.y + 2);
    CHECK(flow.object(3)->x == rock.x + 5);
    CHECK(flow.object(1)->height == 2);  // keeps its height above the ground
    CHECK(flow.object(1)->z == Approx(groundHeight(parsed(testMap()), tree.x + 5, tree.y + 2) + 2));
    CHECK(flow.selection() == Ids{1, 3});
    // One step: a single undo puts both back.
    flow.send("map.undo");
    CHECK(flow.object(1)->x == tree.x);
    CHECK(flow.object(3)->x == rock.x);
    CHECK(flow.selection() == Ids{1, 3});
    flow.send("map.redo");
    CHECK(flow.object(3)->x == rock.x + 5);

    // Esc during a drag cancels it.
    flow.send("map.press\t20\t10\t\t3");
    flow.send("map.drag\t40\t10");
    flow.send("edit.cancel");
    flow.send("map.release\t");
    CHECK(flow.object(3)->x == rock.x + 5);
}

TEST_CASE("Flow: Delete, turn, copy, paste and duplicate act on the whole selection") {
    Flow flow("flow-clipboard");
    flow.send("placement.template\tTestTank");
    flow.send("map.click_right\t10\t10\t");  // 3
    flow.send("map.click_right\t20\t10\t");  // 4
    flow.send("selection.set\t3,4");

    // [ ] turn the selection about its middle.
    flow.send("edit.rotate\t90");
    CHECK(flow.object(3)->x == Approx(15).margin(1e-4));
    CHECK(flow.object(3)->y == Approx(5).margin(1e-4));
    CHECK(flow.object(4)->y == Approx(15).margin(1e-4));
    CHECK(flow.object(3)->angle == Approx(3.14159265 / 2));
    flow.send("map.undo");
    CHECK(flow.object(3)->x == Approx(10));
    CHECK(flow.object(3)->angle == Approx(0));

    // Copy, paste at the cursor: same layout around it, new uniqueIDs, selected.
    flow.send("edit.copy");
    CHECK(flow.editor().clipboardCount == 2);
    flow.send("edit.paste\t25\t5");
    auto objects = flow.objects();
    REQUIRE(objects.size() == 6);
    CHECK(objects[4].x == Approx(20));
    CHECK(objects[4].y == Approx(5));
    CHECK(objects[5].x == Approx(30));
    CHECK(objects[4].templateName == "TestTank");
    CHECK(flow.selection() == Ids{5, 6});
    CHECK(flow.property(5, "uniqueID") != flow.property(3, "uniqueID"));
    CHECK(flow.property(5, "originalOwner") == flow.property(3, "originalOwner"));
    CHECK(flow.reader->get<MapRow>(1).undoCount == 3);  // two placements and the paste, one step
    // Paste with the cursor off the map: the map's middle.
    flow.send("edit.paste\t\t");
    objects = flow.objects();
    REQUIRE(objects.size() == 8);
    CHECK(objects[6].x == Approx(10));  // middle (15, 10) - 5
    CHECK(objects[6].y == Approx(10));
    // Duplicate: next to the original, the clipboard stays.
    flow.send("selection.set\t3");
    flow.send("edit.duplicate");
    objects = flow.objects();
    REQUIRE(objects.size() == 9);
    CHECK(objects[8].x == Approx(30));
    CHECK(objects[8].y == Approx(-10));
    CHECK(flow.editor().clipboardCount == 2);
    // Waypoints are not copied.
    flow.send("selection.set\t2");
    CHECK(flow.refused("edit.copy").empty());
    CHECK(flow.editor().noteKind == "error");
    CHECK(flow.editor().clipboardCount == 2);

    // Delete removes the selection as one step; with nothing selected it says so.
    flow.send("selection.set\t5,6,7");
    flow.send("edit.delete");
    CHECK(flow.objects().size() == 6);
    CHECK(flow.selection().empty());
    flow.send("edit.delete");
    CHECK_FALSE(flow.editor().note.empty());
    flow.send("map.undo");
    CHECK(flow.objects().size() == 9);
    CHECK(flow.selection() == Ids{5, 6, 7});
}

TEST_CASE("Flow: the inspector edits owner, height and properties of the selection") {
    Flow flow("flow-inspector");
    flow.send("selection.set\t1");
    flow.send("selection.height\t25");
    CHECK(flow.object(1)->height == 25);
    flow.send("selection.property\tobjectInitialHealth\tint\t40");
    flow.send("selection.property\tobjectName\tstring\tBigOak");
    flow.send("selection.property\tobjectScriptAttachment\tstring\tTreeScript");
    flow.send("selection.property\tobjectIndestructible\tbool\tfalse");
    flow.send("selection.property\tobjectName\tunicode\tÖak");  // the same key, another type
    flow.send("selection.unset\tobjectScale");
    CHECK(flow.property(1, "objectInitialHealth") == "int:40");
    CHECK(flow.property(1, "objectName") == "unicode:Öak");
    CHECK(flow.property(1, "objectIndestructible") == "bool:false");
    CHECK(flow.property(1, "objectScale") == "<none>");
    // Bad values are refused with a note; nothing changes.
    const auto steps = flow.reader->get<MapRow>(1).undoCount;
    flow.send("selection.property\tobjectInitialHealth\tint\tlots");
    CHECK(flow.editor().noteKind == "error");
    flow.send("selection.height\t100000");
    CHECK(flow.editor().noteKind == "error");
    flow.send("selection.owner\tteamNobody");
    CHECK(flow.editor().noteKind == "error");
    CHECK(flow.reader->get<MapRow>(1).undoCount == steps);
    CHECK(flow.refused("selection.property\tx\tcolor\t1") == "error:bad_argument");

    // A move keeps the height; the file has all of it.
    flow.send("map.press\t15\t5\t\t1");
    flow.send("map.drag\t17\t5");
    flow.send("map.release\t");
    CHECK(flow.object(1)->x == 17);
    CHECK(flow.object(1)->height == 25);
    auto file = flow.save("inspected");
    REQUIRE(file.objects.size() == 2);
    const auto& tree = file.objects[0];
    CHECK(tree.x == 17);
    CHECK(tree.z == 25);
    CHECK(findDictValue(tree.properties, "objectInitialHealth")->data == DictValue::Data(std::int32_t(40)));
    CHECK(findDictValue(tree.properties, "objectName")->text() == "Öak");
    CHECK(std::holds_alternative<UnicodeText>(findDictValue(tree.properties, "objectName")->data));
    CHECK(findDictValue(tree.properties, "objectScriptAttachment")->text() == "TreeScript");
    CHECK_FALSE(findDictValue(tree.properties, "objectScale"));
    // The waypoint and every chunk but ObjectsList are the original bytes.
    const auto original = parsed(testMap());
    CHECK(
        file.data.substr(file.objectChunks[1].header + 4, file.objectChunks[1].end - file.objectChunks[1].header - 4) ==
        original.data.substr(original.objectChunks[1].header + 4,
                             original.objectChunks[1].end - original.objectChunks[1].header - 4));
    for (std::size_t i = 0; i < file.chunks.size(); ++i) {
        if (file.chunks[i].name != "ObjectsList") {
            CHECK(file.data.substr(file.chunks[i].header + 4, file.chunks[i].end - file.chunks[i].header - 4) ==
                  original.data.substr(original.chunks[i].header + 4,
                                       original.chunks[i].end - original.chunks[i].header - 4));
        }
    }

    // Undoing every step gives the original objects back, byte for byte.
    while (flow.reader->get<MapRow>(1).undoCount > 0) {
        flow.send("map.undo");
    }
    file = flow.save("undone");
    CHECK(
        file.data.substr(file.objectChunks[0].header + 4, file.objectChunks[0].end - file.objectChunks[0].header - 4) ==
        original.data.substr(original.objectChunks[0].header + 4,
                             original.objectChunks[0].end - original.objectChunks[0].header - 4));

    // Owner: a team of the map; the object takes its player's color.
    flow.send("placement.template\tTestRock");
    flow.send("map.click_right\t5\t5\t");
    flow.send("selection.owner\tteamPlyrRed");
    CHECK(flow.object(3)->owner == "teamPlyrRed");
    CHECK(flow.object(3)->color == 0xFF0000);
}

TEST_CASE("Flow: waypoints are placed as linked paths") {
    Flow flow("flow-waypoints");
    flow.send("placement.waypoint");
    CHECK(flow.editor().tool == "waypoint");
    flow.send("selection.set\t");
    flow.send("map.click_right\t5\t5\t");  // 3: a new path
    flow.send("map.click_right\t10\t5\t");  // 4, linked from 3
    flow.send("map.click_right\t15\t5\t");  // 5, linked from 4
    auto objects = flow.objects();
    REQUIRE(objects.size() == 5);
    CHECK(objects[2].kind == "waypoint");
    CHECK(objects[2].templateName == "*Waypoints/Waypoint");
    CHECK(flow.property(3, "waypointID") == "int:2");  // the test map has waypoint 1
    CHECK(flow.property(3, "waypointName") == "string:Waypoint 2");
    CHECK(flow.property(5, "waypointName") == "string:Waypoint 4");
    auto links = flow.links();
    REQUIRE(links.size() == 2);
    CHECK(links[0].kind == "waypoint");
    CHECK(links[0].fromId == 3);
    CHECK(links[0].toId == 4);
    CHECK(links[1].fromId == 4);
    CHECK(links[1].toId == 5);
    // Each placement with its link is one step.
    CHECK(flow.reader->get<MapRow>(1).undoCount == 3);
    // A right click on another waypoint links the selected one to it, a second time unlinks.
    flow.send("map.click_right\t5\t15\t2");
    CHECK(flow.links().size() == 3);
    CHECK(flow.links().back().fromId == 5);
    CHECK(flow.links().back().toId == 2);
    CHECK(flow.selection() == Ids{2});
    flow.send("selection.set\t5");
    flow.send("map.click_right\t5\t15\t2");
    CHECK(flow.links().size() == 2);
    // Deleting a waypoint takes its links; undo brings them back.
    flow.send("selection.set\t4");
    flow.send("edit.delete");
    CHECK(flow.links().empty());
    flow.send("map.undo");
    CHECK(flow.links().size() == 2);

    const auto file = flow.save("waypoints");
    CHECK(file.waypointLinks == std::vector<std::array<std::int32_t, 2>>{{2, 3}, {3, 4}});
    CHECK(file.chunkNames.back() == "WaypointsList");  // the test map had none
    std::size_t waypoints = 0;
    for (const auto& object: file.objects) {
        waypoints += findDictValue(object.properties, "waypointID") ? 1 : 0;
    }
    CHECK(waypoints == 4);
}

TEST_CASE("Flow: roads and bridges are segments of two points of a Roads.ini type") {
    Flow flow("flow-roads");
    CHECK(flow.refused("placement.road\tNoRoad") == "error:unknown_road");
    CHECK(flow.refused("placement.road\tWoodBridge") == "error:unknown_road");  // a bridge, not a road
    CHECK(flow.reader->count<RoadTypeRow>() == 2);
    flow.send("placement.road\tTwoLane");
    flow.send("map.click_right\t2\t2\t");
    auto editor = flow.editor();
    CHECK(editor.roadStart);
    CHECK(editor.roadX == 2);
    CHECK(flow.objects().size() == 2);
    flow.send("map.click_right\t28\t2\t");
    CHECK_FALSE(flow.editor().roadStart);
    auto objects = flow.objects();
    REQUIRE(objects.size() == 4);
    CHECK(objects[2].templateName == "TwoLane");
    CHECK(objects[2].kind == "road");
    CHECK(objects[2].flags == mapObjectRoadPoint1);
    CHECK(objects[3].flags == mapObjectRoadPoint2);
    CHECK(objects[3].x == 28);
    CHECK(flow.property(3, "originalOwner") == "string:team");
    CHECK(flow.selection() == Ids{3, 4});
    REQUIRE(flow.links().size() == 1);
    CHECK(flow.links()[0].kind == "road");
    CHECK(flow.reader->get<MapRow>(1).undoCount == 1);

    flow.send("placement.bridge\tWoodBridge");
    flow.send("map.click_right\t5\t18\t");
    flow.send("edit.delete");  // Delete cancels the started segment
    CHECK_FALSE(flow.editor().roadStart);
    flow.send("map.click_right\t5\t18\t");
    flow.send("map.click_right\t5\t18\t");
    CHECK(flow.editor().noteKind == "error");  // no zero-length segment
    flow.send("map.click_right\t25\t18\t");
    objects = flow.objects();
    REQUIRE(objects.size() == 6);
    CHECK(objects[4].flags == mapObjectBridgePoint1);
    CHECK(objects[5].flags == mapObjectBridgePoint2);
    CHECK(flow.links().back().kind == "bridge");

    // Moving one end shapes the road; deleting one end deletes the segment.
    flow.send("selection.set\t4");
    flow.send("map.press\t28\t2\t\t4");
    flow.send("map.drag\t28\t6");
    flow.send("map.release\t");
    CHECK(flow.object(4)->y == 6);
    CHECK(flow.object(3)->y == 2);
    flow.send("selection.set\t5");
    flow.send("edit.delete");
    CHECK_FALSE(flow.object(5));
    CHECK_FALSE(flow.object(6));
    flow.send("map.undo");
    // Copying one end copies the segment.
    flow.send("selection.set\t3");
    flow.send("edit.copy");
    CHECK(flow.editor().clipboardCount == 2);

    const auto file = flow.save("roads");
    REQUIRE(file.objects.size() == 6);
    CHECK(file.objects[2].templateName == "TwoLane");
    CHECK(file.objects[2].flags == mapObjectRoadPoint1);
    CHECK(file.objects[3].flags == mapObjectRoadPoint2);
    CHECK(file.objects[5].flags == mapObjectBridgePoint2);
}

TEST_CASE("Flow: Save Map As writes the map's preview next to it") {
    Flow flow("flow-preview");
    flow.save("previewed");
    const auto tga = readFile(flow.saved / "previewed.tga");
    REQUIRE(tga.size() == 18 + 128 * 128 * 4 + 26);  // like the shipped previews
    CHECK(tga[2] == 2);
    CHECK(std::uint8_t(tga[12]) == 128);
    CHECK(std::uint8_t(tga[14]) == 128);
    CHECK(tga[16] == 32);
    CHECK(tga.substr(tga.size() - 18, 17) == "TRUEVISION-XFILE.");
    CHECK(flow.editor().note == "Saved " + (flow.saved / "previewed.map").string());
}

TEST_CASE("Map preview: tile colors lit by height, water areas blue") {
    // 9 x 9 flat cells at height 0 with one tile class, a water area over the west half.
    ChunkWriter w;
    w.open("HeightMapData", 4);
    w.body.u32(9).u32(9).u32(0).u32(1).u32(8).u32(8).u32(81);
    for (int i = 0; i < 81; ++i) {
        w.body.u8(std::uint8_t(i % 9 < 5 ? 0 : 40));  // the east is higher
    }
    w.close();
    w.open("PolygonTriggers", 4);
    w.body.u32(1);
    w.ascii("Lake");
    w.ascii("");
    w.body.u32(1).u8(1).u8(0).u32(0).u32(4);
    for (const auto& point: std::vector<std::array<int, 2>>{{0, 0}, {35, 0}, {35, 80}, {0, 80}}) {
        w.body.u32(std::uint32_t(point[0])).u32(std::uint32_t(point[1])).u32(10);
    }
    w.close();
    auto map = parsed(w.file());
    map.tiles.assign(81, 0);  // every cell shows tile 0
    const std::vector<std::array<float, 3>> colors(1, {0.5f, 0.4f, 0.3f});
    const auto rgba = renderMapPreview(map, colors);
    REQUIRE(rgba.size() == 128 * 128 * 4);
    const auto pixel = [&](int x, int y) {
        return &rgba[(std::size_t(y) * 128 + std::size_t(x)) * 4];
    };
    CHECK(pixel(10, 64)[2] > pixel(10, 64)[0]);  // west: water, blue
    CHECK(pixel(120, 64)[0] > pixel(120, 64)[2]);  // east: the tile's warm color
    CHECK(pixel(120, 64)[3] == 255);
    const auto tga = encodeTga(128, 128, rgba);
    CHECK(std::uint8_t(tga[18]) == pixel(0, 0)[2]);  // BGRA, bottom row first
    CHECK(std::uint8_t(tga[20]) == pixel(0, 0)[0]);
}

namespace {
    // A stand-in for the game app: Fake.app/Contents/MacOS/run.sh writes the arguments it got (one
    // per line), CNC_GENERALS_ZH_PATH, CNC_GENERALS_PATH and GENERALSX_USER_DATA_DIR to files next to the app, then waits
    // `seconds` in a child process (like run.sh, which runs the game as its child) and exits with `code`.
    std::filesystem::path
    makeFakeGame(const std::filesystem::path& folder, int seconds = 0, int code = 0, bool ignoreTerm = false) {
        const auto app = folder / "Fake.app";
        std::filesystem::create_directories(app / "Contents" / "MacOS");
        const auto script = app / "Contents" / "MacOS" / "run.sh";
        std::ofstream(script) << "#!/bin/sh\n"
                              << (ignoreTerm ? "trap '' TERM\n" : "")
                              << "for a in \"$@\"; do printf '%s\\n' \"$a\"; done > '" << (folder / "args.txt").string()
                              << "'\n"
                              << "printf '%s\\n%s\\n%s\\n' \"$CNC_GENERALS_ZH_PATH\" \"$CNC_GENERALS_PATH\" "
                                 "\"$GENERALSX_USER_DATA_DIR\" > '"
                              << (folder / "env.txt").string() << "'\n"
                              << "echo started $$ >> '" << (folder / "runs.txt").string() << "'\n"
                              << "sleep " << seconds << " &\necho $! > '" << (folder / "child.txt").string()
                              << "'\nwait $!\nexit " << code << "\n";
        std::filesystem::permissions(script, std::filesystem::perms::owner_all);
        return app;
    }

    std::vector<std::string> lines(const std::string& text) {
        std::vector<std::string> result;
        std::size_t start = 0;
        while (start < text.size()) {
            const auto end = text.find('\n', start);
            result.push_back(text.substr(start, end - start));
            start = end == std::string::npos ? text.size() : end + 1;
        }
        return result;
    }

    // Runs the core's updates (which watch the started game) until the play row leaves "running".
    PlayRow waitForGame(Flow& flow, double seconds = 10) {
        const auto start = std::chrono::steady_clock::now();
        while (flow.reader->get<PlayRow>(1).state == "running" &&
               std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds)) {
            end_editor_update();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return flow.reader->get<PlayRow>(1);
    }
}

TEST_CASE("Flow: Play saves the map with its unsaved edits to the test copy and starts the game on it") {
    Flow flow("flow-play");
    const auto userMaps = flow.saved / "Maps";
    flow.send("maps.user\t" + userMaps.string());
    const auto app = makeFakeGame(flow.saved);
    flow.send("play.app\t" + app.string());
    flow.send("play.faction\tFactionChina");
    flow.send("play.ai\t2");
    flow.send("play.difficulty\thard");
    flow.send("play.start\t2");
    flow.send("placement.template\tTestTank");
    flow.send("map.click_right\t20\t30\t");
    const auto placed = flow.objects().size();
    REQUIRE(flow.reader->get<MapRow>(1).edited);

    flow.send("map.play");
    auto play = flow.reader->get<PlayRow>(1);
    const auto testCopy = userMaps / "__EditorTest" / "__EditorTest.map";
    CHECK(play.state == "running");
    CHECK(play.pid > 0);
    CHECK(play.runs == 1);
    CHECK(play.mapPath == testCopy.string());
    const auto program = (app / "Contents" / "MacOS" / "run.sh").string();
    CHECK(play.command ==
          program + "\t-testMap\t" + testCopy.string() +
              "\t-testMapFaction\tFactionChina\t-testMapAI\t2\t-testMapAIDifficulty\thard\t-testMapStart\t2");
    CHECK(flow.editor().note.rfind("Playing Test Map in Fake.app", 0) == 0);
    // The test copy has the unsaved edit; the user's map stays unsaved and edited.
    const auto copy = parsed(readFile(testCopy));
    CHECK(copy.objects.size() == placed);
    CHECK(copy.objects.back().templateName == "TestTank");
    CHECK(readFile(userMaps / "__EditorTest" / "__EditorTest.tga").size() == 18 + 128 * 128 * 4 + 26);
    CHECK(flow.reader->get<MapRow>(1).edited);
    CHECK(flow.reader->get<MapRow>(1).savedPath.empty());
    // It is not listed as one of the user's maps.
    flow.send("maps.user\t" + userMaps.string());
    for (const auto& row: flow.reader->iterate<MapListRow>()) {
        CHECK(row.name != "__EditorTest");
    }

    play = waitForGame(flow);
    CHECK(play.state == "exited");
    CHECK(play.exitCode == 0);
    CHECK(play.pid == 0);
    // What the game got: exactly the command line, and the editor's game folder as its data.
    CHECK(lines(readFile(flow.saved / "args.txt")) == std::vector<std::string>{"-testMap",
                                                                               testCopy.string(),
                                                                               "-testMapFaction",
                                                                               "FactionChina",
                                                                               "-testMapAI",
                                                                               "2",
                                                                               "-testMapAIDifficulty",
                                                                               "hard",
                                                                               "-testMapStart",
                                                                               "2"});
    const auto environment = lines(readFile(flow.saved / "env.txt"));
    REQUIRE(environment.size() == 3);
    CHECK(environment[0] == flow.folder.string());
    CHECK(environment[2] == flow.saved.string());  // the user data folder whose Maps has the test copy
}

TEST_CASE("Flow: Play again closes the game the last Play started; play.stop closes it") {
    Flow flow("flow-play-again");
    flow.send("maps.user\t" + (flow.saved / "Maps").string());
    flow.send("play.app\t" + makeFakeGame(flow.saved, 30).string());
    flow.send("map.play");
    const auto first = flow.reader->get<PlayRow>(1);
    REQUIRE(first.state == "running");
    flow.send("map.play");
    const auto second = flow.reader->get<PlayRow>(1);
    CHECK(second.state == "running");
    CHECK(second.runs == 2);
    CHECK(second.pid != first.pid);
    CHECK(::kill(pid_t(first.pid), 0) != 0);  // the first one is gone
    flow.send("play.stop");
    const auto stopped = waitForGame(flow);
    CHECK(stopped.state == "exited");
    CHECK(stopped.exitCode == 128 + SIGTERM);
}

TEST_CASE("Flow: a game that does not quit when asked is ended (Play again, play.stop)") {
    Flow flow("flow-play-stubborn");
    flow.send("maps.user\t" + (flow.saved / "Maps").string());
    flow.send("play.app\t" + makeFakeGame(flow.saved, 30, 0, true).string());
    flow.send("map.play");
    const auto first = flow.reader->get<PlayRow>(1);
    REQUIRE(first.state == "running");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // its trap is set
    flow.send("map.play");
    CHECK(flow.reader->get<PlayRow>(1).runs == 2);
    CHECK(::kill(pid_t(first.pid), 0) != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto child = std::stoi(readFile(flow.saved / "child.txt"));
    CHECK(::kill(pid_t(child), 0) == 0);
    flow.send("play.stop");
    const auto stopped = waitForGame(flow, 10);
    CHECK(stopped.state == "exited");
    CHECK(stopped.exitCode == 128 + SIGKILL);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(::kill(pid_t(child), 0) != 0);  // the game the launcher script ran is gone too
}

TEST_CASE("Flow: Play settings: defaults, refused values, kept across sessions") {
    Flow flow("flow-play-settings");
    auto play = flow.reader->get<PlayRow>(1);
    CHECK(play.app.empty());
    CHECK(play.faction == "FactionAmerica");
    CHECK(play.aiCount == 1);
    CHECK(play.aiDifficulty == "medium");
    CHECK(play.startPosition == 1);
    CHECK(play.state == "idle");
    const char* home = std::getenv("HOME");
    REQUIRE(home);
    CHECK(play.appResolved == (std::filesystem::path(home) / "GeneralsX/Apps/GeneralsXZH.app").string());
    // The base game's user Maps folder means the base game's app.
    flow.send("maps.user\t" + (flow.saved / "GeneralsX" / "Generals" / "Maps").string());
    CHECK(flow.reader->get<PlayRow>(1).appResolved ==
          (std::filesystem::path(home) / "GeneralsX/Apps/GeneralsX.app").string());

    // The playable factions of PlayerTemplate.ini, what the faction menu offers.
    std::vector<std::string> factions;
    for (const auto& row: flow.reader->iterate<FactionRow>(sqlite_orm::order_by(&FactionRow::id))) {
        factions.push_back(row.name + "/" + row.side);
    }
    CHECK(factions == std::vector<std::string>{"FactionAmerica/America", "FactionChina/China"});
    CHECK(flow.refused("play.faction\tFactionCivilian") == "error:unknown_faction");
    CHECK(flow.editor().noteKind == "error");
    CHECK(flow.refused("play.ai\t9") == "error:bad_argument");
    CHECK(flow.refused("play.start\tx") == "error:bad_argument");
    CHECK(flow.refused("play.difficulty\tinsane") == "error:bad_argument");
    flow.send("play.faction\tRandom");
    flow.send("play.ai\t0");
    flow.send("play.start\t0");
    flow.send("play.difficulty\teasy");
    flow.send("play.app\t/nowhere/Game.app");

    // Without a game app there: the map is still written, the store says what is missing.
    flow.send("maps.user\t" + (flow.saved / "Maps").string());
    flow.send("map.play");
    play = flow.reader->get<PlayRow>(1);
    CHECK(play.state == "error");
    CHECK(play.message == "error: no game app at /nowhere/Game.app (choose it in the Play settings)");
    CHECK(flow.editor().note == play.message);

    // The settings stay for the next session; the state does not.
    end_editor_shutdown();
    REQUIRE(end_editor_initialize(flow.dbPath.c_str()));
    play = flow.reader->get<PlayRow>(1);
    CHECK(play.app == "/nowhere/Game.app");
    CHECK(play.faction == "Random");
    CHECK(play.aiCount == 0);
    CHECK(play.startPosition == 0);
    CHECK(play.aiDifficulty == "easy");
    CHECK(play.state == "idle");
    CHECK(flow.refused("map.play") == "error:no_map");
}

TEST_CASE("Flow: Save Map As a name: into the user Maps folder, asking before it replaces another map") {
    Flow flow("flow-save-as");
    const auto userMaps = flow.saved / "Maps";
    CHECK(flow.refused("map.save_as\tMine") == "error:no_user_maps");
    flow.send("maps.user\t" + userMaps.string());
    CHECK(flow.refused("map.save_as\t") == "error:missing_argument");
    CHECK(flow.refused("map.save_as\ta/b") == "error:bad_argument");
    CHECK(flow.editor().noteKind == "error");

    flow.send("placement.template\tTestRock");
    flow.send("map.click_right\t10\t10\t");
    flow.send("map.save_as\tMine");
    const auto mine = userMaps / "Mine" / "Mine.map";
    auto map = flow.reader->get<MapRow>(1);
    CHECK(map.savedPath == mine.string());
    CHECK_FALSE(map.edited);
    CHECK(map.saveConflict.empty());
    CHECK(readFile(userMaps / "Mine" / "Mine.tga").size() == 18 + 128 * 128 * 4 + 26);
    const auto saved = readFile(mine);
    // Its own file again: no question.
    flow.send("map.click_right\t30\t10\t");
    flow.send("map.save_as\tMine");
    CHECK(flow.reader->get<MapRow>(1).saveConflict.empty());
    CHECK(readFile(mine) != saved);

    // Another map's name: not replaced until asked.
    std::filesystem::create_directories(userMaps / "Other");
    std::ofstream(userMaps / "Other" / "Other.map") << "someone else's map";
    flow.send("map.save_as\tOther");
    map = flow.reader->get<MapRow>(1);
    CHECK(map.saveConflict == (userMaps / "Other" / "Other.map").string());
    CHECK(map.savedPath == mine.string());
    CHECK(flow.editor().noteKind == "error");
    CHECK(readFile(userMaps / "Other" / "Other.map") == "someone else's map");
    flow.send("map.save_as\tOther\treplace");
    map = flow.reader->get<MapRow>(1);
    CHECK(map.saveConflict.empty());
    CHECK(map.savedPath == (userMaps / "Other" / "Other.map").string());
    CHECK(parsed(readFile(userMaps / "Other" / "Other.map")).objects.size() == flow.objects().size());
    // Both are listed as user maps.
    std::vector<std::string> names;
    for (const auto& row: flow.reader->iterate<MapListRow>()) {
        if (row.source == "User maps") {
            names.push_back(row.name);
        }
    }
    CHECK(names == std::vector<std::string>{"Mine", "Other"});
}
