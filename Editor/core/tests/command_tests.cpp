// The client's text commands read into typed values (commands::parse), and refused at the C ABI
// when malformed: the error comes back from end_editor_send() and nothing is queued.
#include "commands.h"
#include "end_editor/editor_core_c.h"
#include "test_big_writer.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using namespace end_editor;
using namespace end_editor::commands;

namespace {
    Command command(std::string_view text) {
        auto parsed = parse(text);
        INFO(std::string(text));
        REQUIRE(std::holds_alternative<Command>(parsed));
        return std::get<Command>(std::move(parsed));
    }
    template<class T>
    T core(std::string_view text) {
        auto parsed = command(text);
        INFO(std::string(text));
        REQUIRE(std::holds_alternative<T>(parsed));
        return std::get<T>(std::move(parsed));
    }
    template<class T>
    T edit(std::string_view text) {
        auto parsed = core<Edit>(text);
        INFO(std::string(text));
        REQUIRE(std::holds_alternative<T>(parsed));
        return std::get<T>(std::move(parsed));
    }
    template<class T>
    T setting(std::string_view text) {
        auto parsed = core<PlaySetting>(text);
        INFO(std::string(text));
        REQUIRE(std::holds_alternative<T>(parsed));
        return std::get<T>(std::move(parsed));
    }
    Error error(std::string_view text) {
        auto parsed = parse(text);
        INFO(std::string(text));
        REQUIRE(std::holds_alternative<Error>(parsed));
        return std::get<Error>(std::move(parsed));
    }
    std::string code(std::string_view text) {
        return error(text).code;
    }
}

TEST_CASE("Numbers are read with the C locale's format and nothing else") {
    CHECK(parseNumber("12") == 12.0);
    CHECK(parseNumber("-12.5") == -12.5);
    CHECK(parseNumber("+3") == 3.0);
    CHECK(parseNumber("1e-05") == 1e-05);
    CHECK(parseNumber(".5") == 0.5);
    CHECK(parseNumber("0.2500") == 0.25);
    CHECK_FALSE(parseNumber(""));
    CHECK_FALSE(parseNumber("x"));
    CHECK_FALSE(parseNumber("1,5"));  // a decimal comma is not a number
    CHECK_FALSE(parseNumber("12 "));
    CHECK_FALSE(parseNumber("12abc"));
    CHECK_FALSE(parseNumber("+-1"));
    CHECK_FALSE(parseNumber("inf"));
    CHECK_FALSE(parseNumber("nan"));
    CHECK_FALSE(parseNumber("1e999"));
}

TEST_CASE("Commands: the game, models and maps") {
    const auto game = core<OpenGame>("game.open\t/zh");
    CHECK(game.folder == "/zh");
    CHECK_FALSE(game.baseFolder);
    CHECK(core<OpenGame>("game.open\t/zh\t/base").baseFolder == "/base");
    CHECK_FALSE(core<OpenGame>("game.open\t/zh\t").baseFolder);
    CHECK(error("game.open") == Error{"error:missing_argument", ""});
    CHECK(code("game.open\t") == "error:missing_argument");

    const auto model = core<OpenModel>("model.open\tAVTank\tMUZZLE\tTURRET01 TURRET02");
    CHECK(model.name == "AVTank");
    CHECK(model.muzzleFlashes == "MUZZLE");
    CHECK(model.hiddenSubObjects == "TURRET01 TURRET02");
    CHECK(core<OpenModel>("model.open\tAVTank").muzzleFlashes.empty());
    CHECK(error("model.open\t") == Error{"error:missing_argument", ""});
    CHECK(core<PlayAnimation>("model.animation\tAVTANK.TURN").name == "AVTANK.TURN");
    CHECK(core<PlayAnimation>("model.animation\t").name.empty());
    CHECK(core<PlayAnimation>("model.animation").name.empty());  // the bind pose

    CHECK(core<OpenMap>("map.open\tmaps/a b/a b.map").path == "maps/a b/a b.map");
    CHECK(error("map.open") == Error{"error:missing_argument", ""});
    CHECK(core<SetUserMaps>("maps.user\t/u/Maps").folder == "/u/Maps");
    CHECK(core<SetUserMaps>("maps.user").folder.empty());
    CHECK(core<SaveMap>("map.save\t/tmp/x.map").file == "/tmp/x.map");
    CHECK(error("map.save\t") == Error{"error:missing_argument", ""});
    core<PlayMap>("map.play");

    const auto saveAs = core<SaveMapAs>("map.save_as\tMine");
    CHECK(saveAs.name == "Mine");
    CHECK_FALSE(saveAs.replace);
    CHECK(core<SaveMapAs>("map.save_as\tMine\treplace").replace);
    CHECK(error("map.save_as\t") == Error{"error:missing_argument", "Type a name for the map"});
    CHECK(error("map.save_as\t . ") == Error{"error:missing_argument", "Type a name for the map"});
    CHECK(error("map.save_as\ta/b") == Error{"error:bad_argument", "A map name cannot have / \\ or : in it"});
    CHECK(code("map.save_as\ta:b") == "error:bad_argument");
    CHECK(code("map.save_as\tMine\tplease") == "error:bad_argument");

    CHECK(error("no.such.command") == Error{"error:unknown_command", ""});
    CHECK(code("") == "error:unknown_command");
    CHECK(code("map.no_such") == "error:unknown_command");
    CHECK(code("edit.no_such") == "error:unknown_command");
}

TEST_CASE("Commands: the Play button's settings") {
    CHECK(setting<SetPlayApp>("play.app\t/Apps/Game.app").app == "/Apps/Game.app");
    CHECK(setting<SetPlayApp>("play.app\t").app.empty());
    CHECK(setting<SetPlayFaction>("play.faction\tFactionChina").faction == "FactionChina");
    CHECK(error("play.faction\t") == Error{"error:missing_argument", "play.faction: missing argument"});
    CHECK(setting<SetPlayColor>("play.color\tColorBlue").color == "ColorBlue");
    CHECK(setting<SetPlayColor>("play.color\tRandom").color == "Random");
    for (const auto* bad: {"play.color\t", "play.color"}) {
        CHECK(error(bad) == Error{"error:missing_argument", "play.color: missing argument"});
    }
    CHECK(setting<SetPlayAiCount>("play.ai\t7").count == 7);
    CHECK(setting<SetPlayAiCount>("play.ai\t0").count == 0);
    CHECK(setting<SetPlayStart>("play.start\t2").position == 2);
    for (const auto* bad: {"play.ai\t8", "play.ai\t-1", "play.ai\t2.5", "play.ai\tx", "play.ai\t", "play.ai"}) {
        CHECK(error(bad) == Error{"error:bad_argument", "play.ai: a whole number from 0 to 7"});
    }
    CHECK(error("play.start\tx") == Error{"error:bad_argument", "play.start: a whole number from 0 to 7"});
    CHECK(setting<SetPlayDifficulty>("play.difficulty\thard").difficulty == "hard");
    CHECK(error("play.difficulty\tinsane") == Error{"error:bad_argument", "play.difficulty: easy, medium or hard"});
    setting<StopPlay>("play.stop");
    CHECK(error("play.nothing") == Error{"error:unknown_command", ""});
}

TEST_CASE("Commands: direct edits") {
    const auto place = edit<PlaceObject>("map.place\tTestRock\t25\t-15.5\t0.5\tteamPlyrRed");
    CHECK(place.templateName == "TestRock");
    CHECK(place.x == 25);
    CHECK(place.y == -15.5f);
    CHECK(place.angle == 0.5f);
    CHECK(place.team == "teamPlyrRed");
    CHECK(edit<PlaceObject>("map.place\tTestRock\t1\t1\t0\t").team.empty());
    CHECK(error("map.place\tTestRock\t1\t1\t0") == Error{"error:missing_argument", "map.place: missing argument"});
    CHECK(code("map.place\t\t1\t1\t0\tteam") == "error:missing_argument");
    CHECK(error("map.place\tTestRock\tx\t1\t0\tteam") == Error{"error:bad_argument", "map.place: not a number"});
    CHECK(code("map.place\tTestRock\t1\t1\t\tteam") == "error:bad_argument");

    const auto move = edit<MoveObject>("map.move\t3\t11\t12");
    CHECK(move.id == 3);
    CHECK(move.x == 11);
    CHECK(move.y == 12);
    CHECK(error("map.move\t1\t2") == Error{"error:missing_argument", "map.move: missing argument"});
    CHECK(code("map.move\t\t1\t2") == "error:missing_argument");
    CHECK(code("map.move\t1\t2\tx") == "error:bad_argument");
    CHECK(code("map.move\tx\t1\t2") == "error:bad_argument");
    CHECK(code("map.move\t1.5\t1\t2") == "error:bad_argument");  // ids are whole numbers
    const auto rotate = edit<RotateObject>("map.rotate\t2\t-1");
    CHECK(rotate.id == 2);
    CHECK(rotate.angle == -1);
    CHECK(code("map.rotate\t2") == "error:missing_argument");
    CHECK(code("map.rotate\t2\tx") == "error:bad_argument");
    CHECK(edit<DeleteObject>("map.delete\t7").id == 7);
    CHECK(code("map.delete\t") == "error:missing_argument");
    CHECK(code("map.delete") == "error:missing_argument");
    CHECK(code("map.delete\tseven") == "error:bad_argument");
    edit<Undo>("map.undo");
    edit<Redo>("map.redo");
}

TEST_CASE("Commands: input from the map view") {
    const auto press = edit<Press>("map.press\t15\t5\tshift,cmd\t1,3");
    REQUIRE(press.at);
    CHECK(*press.at == Point{15, 5});
    CHECK(press.modifiers.shift);
    CHECK_FALSE(press.modifiers.alt);
    CHECK(press.modifiers.cmd);
    CHECK(press.hits == Ids{1, 3});
    const auto offMap = edit<Press>("map.press\t\t\t\t");
    CHECK_FALSE(offMap.at);
    CHECK(offMap.hits.empty());
    CHECK_FALSE(edit<Press>("map.press").at);
    CHECK(edit<Press>("map.press\t0\t0\talt\t").modifiers.alt);
    CHECK(error("map.press\tx\t1\t\t") == Error{"error:bad_argument", "map.press: not a number"});
    CHECK(code("map.press\t1\t\t\t") == "error:bad_argument");  // half a point
    CHECK(code("map.press\t1\t1\tctrl\t") == "error:bad_argument");
    CHECK(error("map.press\t1\t1\t\t1,x") == Error{"error:bad_argument", "map.press: not a list of object ids"});

    CHECK(*edit<Drag>("map.drag\t17\t5").at == Point{17, 5});
    CHECK_FALSE(edit<Drag>("map.drag\t\t").at);
    CHECK(code("map.drag\t17\ty") == "error:bad_argument");

    CHECK(edit<Release>("map.release\t5,6,7").band == Ids{5, 6, 7});
    CHECK(edit<Release>("map.release\t").band.empty());
    CHECK(edit<Release>("map.release").band.empty());
    CHECK(code("map.release\t5,-6") == "error:bad_argument");
    CHECK(code("map.release\t0") == "error:bad_argument");

    const auto click = edit<RightClick>("map.click_right\t20\t4\t3");
    CHECK(*click.at == Point{20, 4});
    CHECK(click.hits == Ids{3});
    CHECK_FALSE(edit<RightClick>("map.click_right\t\t\t").at);
    CHECK(error("map.click_right\tx\t1\t") == Error{"error:bad_argument", "map.click_right: not a number"});
    CHECK(code("map.click_right\t1\t1\tthree") == "error:bad_argument");
}

TEST_CASE("Commands: edits of the selection and the clipboard") {
    edit<DeleteSelection>("edit.delete");
    CHECK(edit<RotateSelection>("edit.rotate\t-15").degrees == -15);
    CHECK(error("edit.rotate\t") == Error{"error:missing_argument", "edit.rotate: missing argument"});
    CHECK(error("edit.rotate\tleft") == Error{"error:bad_argument", "edit.rotate: not a number"});
    edit<Copy>("edit.copy");
    CHECK(*edit<Paste>("edit.paste\t25\t5").at == Point{25, 5});
    CHECK_FALSE(edit<Paste>("edit.paste\t\t").at);  // the middle of the map
    CHECK_FALSE(edit<Paste>("edit.paste").at);
    CHECK(code("edit.paste\t25") == "error:bad_argument");
    edit<Duplicate>("edit.duplicate");
    edit<Cancel>("edit.cancel");
    edit<SelectAll>("edit.select_all");
}

TEST_CASE("Commands: the placement tool") {
    edit<UseObjectTool>("placement.object");
    CHECK(edit<ChooseTemplate>("placement.template\tTestRock").templateName == "TestRock");
    CHECK(error("placement.template\t") == Error{"error:missing_argument", "placement.template: missing argument"});
    edit<UseWaypointTool>("placement.waypoint");
    const auto road = edit<UseRoadTool>("placement.road\tTwoLane");
    CHECK_FALSE(road.bridge);
    CHECK(road.type == "TwoLane");
    const auto bridge = edit<UseRoadTool>("placement.bridge");
    CHECK(bridge.bridge);
    CHECK(bridge.type.empty());  // the last or the first one
    CHECK(edit<SetPlacementOwner>("placement.owner\tteamPlyrRed").team == "teamPlyrRed");
    CHECK(error("placement.owner\t") == Error{"error:missing_argument", "placement.owner: missing argument"});
    CHECK(edit<SetPlacementAngle>("placement.angle\t90.0").degrees == 90);
    CHECK(code("placement.angle\tninety") == "error:bad_argument");
    CHECK(code("placement.angle") == "error:missing_argument");
    edit<ShowPicker>("placement.pick");
    edit<CancelPlacement>("placement.cancel");
    edit<ClearNote>("note.clear");
}

TEST_CASE("Commands: the inspector") {
    CHECK(edit<SetSelection>("selection.set\t1,3").ids == Ids{1, 3});
    CHECK(edit<SetSelection>("selection.set\t,4,,5,").ids == Ids{4, 5});
    CHECK(edit<SetSelection>("selection.set\t").ids.empty());
    CHECK(error("selection.set\tall") == Error{"error:bad_argument", "selection.set: not a list of object ids"});
    CHECK(edit<SetOwner>("selection.owner\tteamPlyrRed").team == "teamPlyrRed");
    CHECK(code("selection.owner\t") == "error:missing_argument");
    CHECK(edit<SetHeight>("selection.height\t25").z == 25);
    CHECK(error("selection.height\thigh") == Error{"error:bad_argument", "selection.height: not a number"});
    CHECK(code("selection.height") == "error:missing_argument");
    CHECK(edit<SetAngle>("selection.angle\t45").degrees == 45);
    CHECK(code("selection.angle\t4,5") == "error:bad_argument");

    const auto property = [](std::string_view text) {
        return edit<SetProperty>(text).value;
    };
    CHECK(property("selection.property\tobjectInitialHealth\tint\t40").data == DictValue::Data(std::int32_t(40)));
    CHECK(property("selection.property\tobjectInitialHealth\tint\t-2147483648").data ==
          DictValue::Data(std::int32_t(-2147483647 - 1)));
    CHECK(property("selection.property\tobjectScale\treal\t1.5").data == DictValue::Data(1.5f));
    CHECK(property("selection.property\tobjectEnabled\tbool\ttrue").data == DictValue::Data(true));
    CHECK(property("selection.property\tobjectEnabled\tbool\t0").data == DictValue::Data(false));
    const auto name = property("selection.property\tobjectName\tstring\tBig Oak");
    CHECK(name.key == "objectName");
    CHECK(name.data == DictValue::Data(AsciiText{"Big Oak"}));
    CHECK(property("selection.property\tobjectName\tunicode\tÖak").data == DictValue::Data(UnicodeText{"Öak"}));
    CHECK(property("selection.property\tobjectName\tstring\t").text().empty());
    CHECK(error("selection.property\tobjectName\tstring") ==
          Error{"error:missing_argument", "selection.property: missing argument"});
    CHECK(code("selection.property\t\tint\t1") == "error:missing_argument");
    CHECK(error("selection.property\tx\tcolor\t1") ==
          Error{"error:bad_argument", "selection.property: unknown type color"});
    CHECK(error("selection.property\tobjectInitialHealth\tint\tlots") ==
          Error{"error:bad_argument", "objectInitialHealth: \"lots\" is not a number"});
    CHECK(code("selection.property\tobjectInitialHealth\tint\t1.5") == "error:bad_argument");
    CHECK(code("selection.property\tobjectInitialHealth\tint\t2147483648") == "error:bad_argument");
    CHECK(code("selection.property\tobjectScale\treal\tbig") == "error:bad_argument");
    CHECK(error("selection.property\tobjectEnabled\tbool\tyes") ==
          Error{"error:bad_argument", "objectEnabled: \"yes\" is not a yes/no value"});
    CHECK(edit<UnsetProperty>("selection.unset\tobjectScale").key == "objectScale");
    CHECK(code("selection.unset\t") == "error:missing_argument");
    edit<LinkWaypoints>("waypoint.link");
}

TEST_CASE("Commands: the placement tool and note.clear need no map") {
    CHECK_FALSE(needsMap(edit<UseRoadTool>("placement.road")));
    CHECK_FALSE(needsMap(edit<ChooseTemplate>("placement.template\tX")));
    CHECK_FALSE(needsMap(edit<ClearNote>("note.clear")));
    CHECK(needsMap(edit<Undo>("map.undo")));
    CHECK(needsMap(edit<SetSelection>("selection.set\t1")));
    CHECK(needsMap(edit<Press>("map.press\t1\t1\t\t")));
}

TEST_CASE("Commands: a malformed command is refused at the C ABI and nothing is queued") {
    const auto folder = end_editor_tests::makeTempFolder("commands-abi");
    REQUIRE(end_editor_initialize((folder / "store.sqlite").string().c_str()));
    for (const auto* text: {"map.move\t1\t2\tx",
                            "map.press\tx\t1\t\t",
                            "selection.property\tobjectInitialHealth\tint\tlots",
                            "play.ai\t9",
                            "map.save_as\ta/b"}) {
        INFO(text);
        CHECK(std::string(end_editor_send(text)) == "error:bad_argument");
        CHECK(end_editor_pending_work() == 0);
    }
    CHECK(std::string(end_editor_send("edit.rotate\t")) == "error:missing_argument");
    CHECK(std::string(end_editor_send("no.such.command")) == "error:unknown_command");
    // Well formed, but no map is open: the state check after parsing.
    CHECK(std::string(end_editor_send("map.move\t1\t2\t3")) == "error:no_map");
    CHECK(std::string(end_editor_send("map.open\tmaps/x/x.map")) == "error:no_game");
    // The placement tool works without a map.
    CHECK(std::string(end_editor_send("placement.angle\t45")).empty());
    CHECK(end_editor_pending_work() == 1);
    while (end_editor_pending_work() > 0) {
        end_editor_update();
    }
    end_editor_shutdown();
}
