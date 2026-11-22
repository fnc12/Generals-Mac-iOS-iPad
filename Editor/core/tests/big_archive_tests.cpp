#include "big_archive.h"
#include "test_big_writer.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>

using namespace end_editor;
using namespace end_editor_tests;

TEST_CASE("BIG archive index and file bodies") {
    const auto folder = makeTempFolder("big");
    const auto path = folder / "Test.big";
    writeBig(path, {{"Data\\INI\\Object\\Tank.ini", "Object Tank\nEnd\n"}, {"Art\\W3D\\tank.w3d", "xyz"}});

    std::string error;
    const auto archive = BigArchive::open(path, error);
    REQUIRE(archive);
    REQUIRE(archive->entries().size() == 2);
    CHECK(archive->entries()[0].path == "data/ini/object/tank.ini");
    CHECK(archive->entries()[1].path == "art/w3d/tank.w3d");

    std::string body;
    REQUIRE(archive->read(archive->entries()[1], body));
    CHECK(body == "xyz");
}

TEST_CASE("Files that are not BIG archives are rejected") {
    const auto folder = makeTempFolder("not-big");
    std::string error;

    SECTION("wrong magic") {
        std::ofstream(folder / "a.big", std::ios::binary) << "ZIPF0000000000000000";
        CHECK_FALSE(BigArchive::open(folder / "a.big", error));
        CHECK(error == "not a BIG archive");
    }
    SECTION("too short") {
        std::ofstream(folder / "b.big", std::ios::binary) << "BIGF";
        CHECK_FALSE(BigArchive::open(folder / "b.big", error));
    }
    SECTION("entry points past the end of the file") {
        std::string data = "BIGF";
        data.append(4, '\0');
        appendBigEndian32(data, 1);
        appendBigEndian32(data, 16 + 8 + 2);
        appendBigEndian32(data, 1000);
        appendBigEndian32(data, 10);
        data += "a";
        data.push_back('\0');
        std::ofstream(folder / "c.big", std::ios::binary) << data;
        CHECK_FALSE(BigArchive::open(folder / "c.big", error));
    }
    SECTION("missing file") {
        CHECK_FALSE(BigArchive::open(folder / "none.big", error));
    }
}
