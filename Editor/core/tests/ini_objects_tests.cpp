#include "ini_objects.h"

#include <catch2/catch_test_macros.hpp>

using namespace end_editor;

namespace {
    // Written in the style of the game's object INI files (made up, not copied).
    constexpr const char* sampleIni = R"(
;------------------------------------------------------------------------------
Object TestBoat
  ; *** ART Parameters ***
  Draw = W3DModelDraw ModuleTag_01
    ConditionState = DAMAGED
      Model = TBoat_D
    End
    DefaultConditionState
      Model = TBoat   ; the healthy model
      Turret = TurretBone
      Animation = TBoat.TBoat
      WeaponMuzzleFlash = PRIMARY GunFX
      WeaponMuzzleFlash = SECONDARY RocketFX
      HideSubObject = Sail Flag01
    End
    ConditionState = REALLYDAMAGED
      HideSubObject = Mast   ; only when damaged
    End
  End
  Draw = W3DModelDraw ModuleTag_Wake
    DefaultConditionState
      Model = TBoatWake
      HideSubObject = NotThisOne
    End
  End
  DisplayName      = OBJECT:TestBoat
  Side             = Civilian
  EditorSorting    = VEHICLE
  Buildable        = No
  KindOf           = PRELOAD SELECTABLE VEHICLE   // comment in C++ style
  ArmorSet
    Conditions = None
    Armor      = BoatArmor
  End
  Behavior = AIUpdateInterface ModuleTag_02
    Turret
      TurretTurnRate = 60
    End
  End
  Body = ActiveBody ModuleTag_03
    MaxHealth = 100
  End
  UnitSpecificSounds
    VoiceMove = NoSound
  End
End

ObjectReskin TestBoatRed TestBoat
  Draw = W3DModelDraw ModuleTag_01
    DefaultConditionState
      Model = TBoatRed
    End
  End
End

ChildObject TestBoatChild TestBoatRed
  Side = America
End

Object TestTree
  Draw = W3DTreeDraw ModuleTag_01
    ModelName = TTree01
    TextureName = TTree01.tga
  End
End
)";
}

TEST_CASE("Object blocks and their fields") {
    const auto result = parseObjects(sampleIni, "data/ini/object/test.ini");
    INFO((result.warnings.empty() ? std::string() : result.warnings.front()));
    REQUIRE(result.warnings.empty());
    REQUIRE(result.objects.size() == 4);

    const auto& boat = result.objects[0];
    CHECK(boat.name == "TestBoat");
    CHECK(boat.definition == "Object");
    CHECK(boat.parent.empty());
    CHECK(boat.side == "Civilian");
    CHECK(boat.kindOf == "PRELOAD SELECTABLE VEHICLE");
    CHECK(boat.buildable == "No");
    CHECK(boat.editorSorting == "VEHICLE");
    CHECK(boat.displayName == "OBJECT:TestBoat");
    CHECK(boat.model == "TBoat");  // default state wins over the damaged one listed first
    CHECK(boat.muzzleFlashes == "GunFX RocketFX");
    CHECK(boat.hiddenSubObjects == "Sail Flag01");  // the second Draw module is not read
    CHECK(boat.sourceFile == "data/ini/object/test.ini");
    CHECK(boat.line == 3);

    CHECK(result.objects[1].definition == "ObjectReskin");
    CHECK(result.objects[1].parent == "TestBoat");
    CHECK(result.objects[1].model == "TBoatRed");
    CHECK(result.objects[2].parent == "TestBoatRed");
    CHECK(result.objects[3].model == "TTree01");  // W3DTreeDraw's ModelName
}

TEST_CASE("Reskins and child objects inherit missing fields") {
    auto objects = parseObjects(sampleIni, "test.ini").objects;
    resolveInheritance(objects);
    const auto& red = objects[1];
    CHECK(red.side == "Civilian");
    CHECK(red.model == "TBoatRed");  // its own model is kept
    const auto& child = objects[2];
    CHECK(child.side == "America");  // its own side is kept
    CHECK(child.model == "TBoatRed");  // from the parent
    CHECK(child.muzzleFlashes == "GunFX RocketFX");  // from the grandparent
    CHECK(child.kindOf == "PRELOAD SELECTABLE VEHICLE");  // from the grandparent
}

TEST_CASE("Broken INI gives warnings, not a crash") {
    SECTION("unclosed object") {
        const auto result = parseObjects("Object A\n  Side = X\n", "a.ini");
        CHECK(result.objects.empty());
        REQUIRE(result.warnings.size() == 1);
        CHECK(result.warnings[0].find("ends inside object A") != std::string::npos);
    }
    SECTION("text outside an object") {
        const auto result = parseObjects("Weapon W\nEnd\nObject B\nEnd\n", "b.ini");
        CHECK(result.objects.size() == 1);
        CHECK(result.warnings.size() == 2);
    }
    SECTION("reskin cycle") {
        std::vector<ObjectInfo> objects(2);
        objects[0].name = "A";
        objects[0].parent = "B";
        objects[1].name = "B";
        objects[1].parent = "A";
        resolveInheritance(objects);  // must return
        CHECK(objects[0].side.empty());
    }
}
