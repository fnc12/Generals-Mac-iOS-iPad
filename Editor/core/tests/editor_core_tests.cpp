#include "editor_core.h"
#include "end_editor/editor_core_c.h"
#include "game_data.h"
#include "test_big_writer.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <map>

using namespace end_editor;
using namespace end_editor_tests;

namespace {
    void makeGameFolder(const std::filesystem::path &folder) {
        // "!" sorts first, so this archive wins over Base.big for the same path.
        writeBig(folder / "!Patch.big", {{"Data\\INI\\Object\\Ships.ini", "Object Ship\n  Side = Patched\nEnd\n"}});
        writeBig(folder / "Base.big",
                 {{"Data\\INI\\Object\\Ships.ini", "Object Ship\n  Side = Old\nEnd\n"},
                  {"Data\\INI\\Object\\Tanks.ini", "Object Tank\n  Side = America\nEnd\nObject Tank2\nEnd\n"},
                  {"Data\\INI\\Weapon.ini", "Object NotAnObject\nEnd\n"},  // not an object file
                  {"Data\\INI\\Default\\Object.ini", "Object DefaultThing\nEnd\n"}});
        std::filesystem::create_directories(folder / "Data" / "INI" / "Object");
        std::ofstream(folder / "Data" / "INI" / "Object" / "Tanks.ini") << "Object LooseTank\n  Side = China\nEnd\n";
    }
}

TEST_CASE("Object INI paths") {
    CHECK(isObjectIniPath("data/ini/object/americavehicle.ini"));
    CHECK(isObjectIniPath("data/ini/default/object.ini"));
    CHECK_FALSE(isObjectIniPath("data/ini/weapon.ini"));
    CHECK_FALSE(isObjectIniPath("data/ini/object/readme.txt"));
}

TEST_CASE("Game folder scan: loose files win, then archives in name order") {
    const auto folder = makeTempFolder("scan");
    makeGameFolder(folder);
    const auto scan = scanGameData(folder);
    CHECK(scan.error.empty());
    CHECK(scan.archiveCount == 2);
    CHECK(scan.iniFileCount == 3);  // ships (patch), tanks (loose), default object

    std::map<std::string, std::string> sides;
    for(const auto &object: scan.objects) {
        sides[object.name] = object.side;
    }
    CHECK(sides.size() == 3);
    CHECK(sides["Ship"] == "Patched");
    CHECK(sides["LooseTank"] == "China");
    CHECK(sides.count("DefaultThing") == 1);
    CHECK(sides.count("Tank") == 0);  // the loose Tanks.ini replaces the archived one
}

TEST_CASE("Game folder scan errors") {
    CHECK_FALSE(scanGameData("/no/such/folder").error.empty());
    CHECK_FALSE(scanGameData(makeTempFolder("empty")).error.empty());
}

TEST_CASE("EditorCore writes the store a client reads") {
    const auto folder = makeTempFolder("core");
    makeGameFolder(folder);
    const auto dbPath = (folder / "store.sqlite").string();

    EditorCore core(dbPath);
    auto status = [&] {
        auto storage = makeStorage(dbPath);  // a second connection, like the client's
        return storage.get<StatusRow>(1);
    };
    CHECK(status().state == "idle");

    CHECK(core.send("game.open") == "error:missing_argument");
    CHECK(core.send("no.such.command") == "error:unknown_command");
    REQUIRE(core.send("game.open\t" + folder.string()).empty());
    CHECK(status().state == "loading");
    const auto loadingRevision = status().revision;

    core.update();
    const auto ready = status();
    CHECK(ready.state == "ready");
    CHECK(ready.objectCount == 3);
    CHECK(ready.gameDir == folder.string());
    CHECK(ready.revision > loadingRevision);

    auto reader = makeStorage(dbPath);
    CHECK(reader.count<ObjectRow>() == 3);

    // Opening a folder without data replaces the list and reports the error.
    REQUIRE(core.send("game.open\t" + (folder / "nothing").string()).empty());
    core.update();
    CHECK(status().state == "error");
    CHECK(reader.count<ObjectRow>() == 0);
}

TEST_CASE("C ABI") {
    const auto folder = makeTempFolder("c-abi");
    makeGameFolder(folder);
    const auto dbPath = (folder / "store.sqlite").string();

    CHECK(std::string(end_editor_send("game.open\tx")) == "error:not_initialized");
    REQUIRE(end_editor_initialize(dbPath.c_str()));
    CHECK_FALSE(end_editor_initialize(dbPath.c_str()));
    CHECK(std::string(end_editor_db_path()) == dbPath);
    CHECK(std::string(end_editor_send(nullptr)) == "error:null_command");
    CHECK(std::string(end_editor_send(("game.open\t" + folder.string()).c_str())).empty());
    end_editor_update();
    end_editor_shutdown();
    CHECK(std::string(end_editor_db_path()).empty());

    auto reader = makeStorage(dbPath);
    CHECK(reader.get<StatusRow>(1).state == "ready");
}
