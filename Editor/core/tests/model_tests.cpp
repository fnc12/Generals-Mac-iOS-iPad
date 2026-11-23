#include "editor_core.h"
#include "model_loader.h"
#include "test_big_writer.h"
#include "test_w3d_writer.h"
#include "texture.h"
#include "w3d.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace end_editor;
using namespace end_editor_tests;
using Catch::Approx;

namespace {
    const float noRotation[4] = {0, 0, 0, 1};
    const float s = std::sqrt(0.5f);  // quaternion of 90 degrees about Z: (0, 0, s, s)

    std::vector<TestPivot> twoBones(bool turned) {
        return {TestPivot{"ROOTTRANSFORM", 0xFFFFFFFFu, {0, 0, 0}, {0, 0, 0, 1}},
                TestPivot{"TURRET", 0, {0, 0, 10}, {0, 0, turned ? s : 0.0f, turned ? s : 1.0f}}};
    }

    // Translation column of a column-major matrix.
    std::array<float, 3> origin(const Matrix4 &m) {
        return {m[12], m[13], m[14]};
    }

    // A tank-like model: a chassis on the root, a turret on bone 1, a separate animation file
    // and a texture that exists both as .tga and .dds.
    void makeModelFolder(const std::filesystem::path &folder) {
        const std::string modelFile = squareMesh("CHASSIS", "TBOX", "TBox.tga") +
                                      squareMesh("TURRET", "TBOX", "TBox.tga") +
                                      squareMesh("FLASH", "TBOX", "", w3d::meshFlagHidden) +
                                      hierarchy("TBOX_SKL", twoBones(false)) +
                                      hlod("TBOX", "TBOX_SKL", {{0, "TBOX.CHASSIS"}, {1, "TBOX.TURRET"}, {1, "TBOX.FLASH"}});
        const std::string animationFile =
            container(0x200, {animationHeader("TBOX_TURN", "TBOX_SKL", 3, false),
                              rawChannel(0, 1, 0, 2, 1, {0, 1, 2})});
        writeBig(folder / "W3D.big", {{"Art\\W3D\\TBox.w3d", modelFile}, {"Art\\W3D\\TBox_Turn.w3d", animationFile}});
        writeBig(folder / "Textures.big",
                 {{"Art\\Textures\\TBox.tga", tga(1, 1, {0, 0, 255, 255})}, {"Art\\Textures\\TBox.dds", solidDds(0x07E0)}});
        writeBig(folder / "INI.big", {{"Data\\INI\\Object\\Box.ini", "Object Box\n  Draw = W3DModelDraw M\n"
                                                                       "    DefaultConditionState\n      Model = TBox\n"
                                                                       "    End\n  End\nEnd\n"}});
    }
}

TEST_CASE("W3D mesh") {
    const auto file = parseW3d(squareMesh("HULL", "BOAT", "Boat.tga"));
    REQUIRE(file.warnings.empty());
    REQUIRE(file.meshes.size() == 1);
    const auto &mesh = file.meshes[0];
    CHECK(mesh.fullName() == "BOAT.HULL");
    CHECK(mesh.vertexCount() == 4);
    CHECK(mesh.indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
    CHECK(mesh.uvs.size() == 8);
    CHECK(mesh.textures == std::vector<std::string>{"Boat.tga"});
    // One id in the file means "this texture for every triangle".
    CHECK(mesh.triangleTextures == std::vector<std::uint32_t>{0, 0});
}

TEST_CASE("Broken W3D gives warnings, not a crash") {
    std::string bytes = squareMesh("HULL", "BOAT", "Boat.tga");
    SECTION("cut short") {
        bytes.resize(bytes.size() / 2);
        CHECK_FALSE(parseW3d(bytes).warnings.empty());
    }
    SECTION("triangle pointing past the vertices") {
        const auto at = bytes.find(Bytes().u32(0x020).u32(2 * 32).data);  // header of the triangle chunk
        REQUIRE(at != std::string::npos);
        bytes[at + 8] = char(99);  // first index of the first triangle
        const auto file = parseW3d(bytes);
        CHECK(file.meshes.at(0).indices.empty());
        CHECK_FALSE(file.warnings.empty());
    }
}

TEST_CASE("Bones: base pose, then animation translation, then rotation") {
    SECTION("bind pose") {
        const auto file = parseW3d(hierarchy("SKL", twoBones(false)));
        REQUIRE(file.hierarchies.size() == 1);
        const auto pose = poseHierarchy(file.hierarchies[0], nullptr, 0);
        REQUIRE(pose.size() == 2);
        CHECK(origin(pose[1]) == std::array<float, 3>{0, 0, 10});
    }
    SECTION("an animated move happens in the bone's own (turned) frame") {
        const auto file = parseW3d(hierarchy("SKL", twoBones(true)) +
                                   container(0x200, {animationHeader("A", "SKL", 2, false),
                                                     rawChannel(0, 1, 1, 1, 1, {5})}));  // X = 5 on frame 1 only
        REQUIRE(file.animations.size() == 1);
        const auto &animation = file.animations[0];
        CHECK(animation.hierarchy == "SKL");
        const auto frame0 = poseHierarchy(file.hierarchies[0], &animation, 0);
        const auto frame1 = poseHierarchy(file.hierarchies[0], &animation, 1);
        CHECK(origin(frame0[1])[0] == Approx(0));
        // turned 90 degrees about Z, the bone's X points along world Y
        CHECK(origin(frame1[1])[0] == Approx(0).margin(1e-5));
        CHECK(origin(frame1[1])[1] == Approx(5));
        CHECK(origin(frame1[1])[2] == Approx(10));
    }
}

TEST_CASE("Animation channels") {
    SECTION("time-coded keys are interpolated, a binary key holds the previous value") {
        const auto file = parseW3d(container(
            0x280, {animationHeader("A", "SKL", 8, true),
                    timeCodedChannel(0, 1, {{0, 0.0f}, {4, 8.0f}, {6u | 0x80000000u, 100.0f}})}));
        REQUIRE(file.animations.size() == 1);
        const auto &x = file.animations[0].translation.at(1);
        CHECK(x[2 * 3] == Approx(4));   // frame 2: halfway from 0 to 8
        CHECK(x[4 * 3] == Approx(8));   // frame 4: the key
        CHECK(x[5 * 3] == Approx(8));   // frame 5: held, the next key is binary
        CHECK(x[7 * 3] == Approx(100));  // after the last key
    }
    SECTION("visibility bits") {
        // frames 1..3 stored, bits 0b101 -> frame 1 on, 2 off, 3 on; outside: default (off)
        const auto file = parseW3d(container(0x200, {animationHeader("A", "SKL", 5, false), rawBitChannel(1, 1, 3, 0, 0b101)}));
        const auto &visible = file.animations.at(0).visible.at(1);
        CHECK(visible == std::vector<std::uint8_t>{0, 1, 0, 1, 0});
    }
    SECTION("the first bytes of a file tell an animation and its skeleton") {
        const auto bytes = container(0x200, {animationHeader("A", "TANK_SKL", 1, false)});
        const auto kind = peekW3dKind(std::string_view(bytes).substr(0, w3dHeaderPeekSize));
        CHECK(kind.kind == W3dFileKind::animationFile);
        CHECK(kind.hierarchy == "TANK_SKL");
    }
}

TEST_CASE("Textures") {
    Image image;
    std::string error;
    SECTION("DXT1") {
        REQUIRE(decodeDds(solidDds(0xF800), image, error));  // pure red in RGB565
        CHECK(image.width == 4);
        CHECK(image.height == 4);
        CHECK(image.rgba[0] == 255);
        CHECK(image.rgba[1] == 0);
        CHECK(image.rgba[3] == 255);
    }
    SECTION("TGA rows are turned top to bottom") {
        // bottom row blue, top row red (BGRA)
        REQUIRE(decodeTga(tga(1, 2, {255, 0, 0, 255, 0, 0, 255, 255}), image, error));
        CHECK(image.rgba[0] == 255);  // first output row is the top (red)
        CHECK(image.rgba[4 + 2] == 255);  // second is blue
    }
    SECTION("unsupported data") {
        CHECK_FALSE(decodeDds("DDS nothing", image, error));
        CHECK_FALSE(decodeTga("tiny", image, error));
    }
}

TEST_CASE("A model with its skeleton, textures and animations") {
    const auto folder = makeTempFolder("model");
    makeModelFolder(folder);
    GameFiles files;
    std::vector<std::string> warnings;
    std::string error;
    REQUIRE(files.mount({folder}, warnings, error));
    ModelLoader loader(files);

    const auto model = loader.load("TBox");
    REQUIRE(model.error.empty());
    REQUIRE(model.parts.size() == 3);
    CHECK(model.parts[0].mesh == "TBOX.CHASSIS");
    CHECK(model.parts[0].bone == 0);
    CHECK(model.parts[1].bone == 1);
    CHECK(model.parts[1].vertices[0].bone == 1);
    CHECK(model.parts[2].hidden);
    CHECK(model.parts[0].vertices[0].uv[1] == Approx(1));  // V flipped like the engine does
    CHECK(model.parts[0].texture == "tbox.tga");
    REQUIRE(model.textures.count("tbox.tga") == 1);
    CHECK(model.textures.at("tbox.tga").width == 4);  // the .dds won over the .tga
    REQUIRE(model.animations.size() == 1);
    CHECK(model.animations[0].name == "TBOX_TURN");
    CHECK(model.boundsMax[2] == Approx(10));  // the turret square sits on the raised bone

    const auto pose = loader.pose(model, "TBOX_TURN", error);
    CHECK(error.empty());
    CHECK(pose.frameCount == 3);
    REQUIRE(pose.matrices.size() == 3 * 2);
    CHECK(origin(pose.matrices[2 * 2 + 1])[0] == Approx(2));  // frame 2, bone 1

    CHECK_FALSE(loader.load("Missing").error.empty());
}

TEST_CASE("EditorCore model commands") {
    const auto folder = makeTempFolder("core-model");
    makeModelFolder(folder);
    const auto dbPath = (folder / "store.sqlite").string();
    EditorCore core(dbPath);
    auto storage = makeStorage(dbPath);

    CHECK(core.send("model.open\tTBox") == "error:no_game");
    REQUIRE(core.send("game.open\t" + folder.string()).empty());
    core.update();
    REQUIRE(core.send("model.open\tTBox").empty());
    CHECK(storage.get<ModelRow>(1).state == "loading");
    core.update();
    const auto row = storage.get<ModelRow>(1);
    INFO(row.message);
    CHECK(row.state == "ready");
    CHECK(row.boneCount == 2);
    CHECK(row.frameCount == 1);
    CHECK(row.pose.size() == 2 * 16 * sizeof(float));
    CHECK(storage.count<ModelPartRow>() == 3);
    CHECK(storage.count<ModelTextureRow>() == 1);
    CHECK(storage.count<ModelAnimationRow>() == 1);

    REQUIRE(core.send("model.animation\tTBOX_TURN").empty());
    core.update();
    CHECK(storage.get<ModelRow>(1).frameCount == 3);
    CHECK(storage.get<ModelRow>(1).animation == "TBOX_TURN");

    SECTION("a later request replaces an earlier one still in the queue") {
        REQUIRE(core.send("model.open\tMissing").empty());
        REQUIRE(core.send("model.open\tTBox").empty());
        core.update();  // skips Missing
        core.update();
        CHECK(storage.get<ModelRow>(1).state == "ready");
        CHECK(storage.get<ModelRow>(1).name == "TBox");
    }
    SECTION("an unknown model") {
        REQUIRE(core.send("model.open\tMissing").empty());
        core.update();
        CHECK(storage.get<ModelRow>(1).state == "error");
        CHECK(storage.count<ModelPartRow>() == 0);
    }
}
