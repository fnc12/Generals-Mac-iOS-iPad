#pragma once

#include "data_chunk.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The client's commands as typed values. A command arrives as tab separated text through the C ABI
// (editor_core_c.h: "name\targ\targ..."); parse() reads and checks all of it once, so the work that
// runs later gets values that are known to be complete and well formed. What depends on the core's
// state (a game or a map being open, a template or road type existing) is checked by
// EditorCore::send after parsing; what depends on the map at the moment an edit runs (a team, an
// object id) by MapEditor.
namespace end_editor::commands {

    using Ids = std::vector<std::int64_t>;  // map_objects ids
    struct Point {  // a ground point (world units)
        float x = 0, y = 0;
        bool operator==(const Point&) const = default;
    };

    // "game.open\t<Zero Hour folder>[\t<base Generals folder>]"
    struct OpenGame {
        std::string folder;
        std::optional<std::string> baseFolder;
    };
    // "model.open\t<model>[\t<muzzle flashes>\t<hidden sub-objects>]" (what the object's Draw
    // module hides while idle)
    struct OpenModel {
        std::string name;
        std::string muzzleFlashes;
        std::string hiddenSubObjects;
    };
    // "model.animation[\t<name>]": "" the bind pose
    struct PlayAnimation {
        std::string name;
    };
    // "map.open\t<path>": a path of the maps table or a .map file
    struct OpenMap {
        std::string path;
    };
    // "maps.user[\t<folder>]": the game's user Maps folder, "" none
    struct SetUserMaps {
        std::string folder;
    };
    // "map.save\t<file>"
    struct SaveMap {
        std::string file;
    };
    // "map.save_as\t<name>[\treplace]": a name in the user Maps folder
    struct SaveMapAs {
        std::string name;
        bool replace = false;
    };
    // "map.play"
    struct PlayMap {};

    // The Play button's settings, "play.*".
    struct SetPlayApp {  // "play.app[\t<app>]": "" the default
        std::string app;
    };
    struct SetPlayFaction {  // "play.faction\t<PlayerTemplate name | Random>"
        std::string faction;
    };
    struct SetPlayColor {  // "play.color\t<MultiplayerColor name | Random>"
        std::string color;
    };
    struct SetPlayAiCount {  // "play.ai\t<0..7>"
        int count = 0;
    };
    struct SetPlayStart {  // "play.start\t<0..7>": 0 any
        int position = 0;
    };
    struct SetPlayDifficulty {  // "play.difficulty\t<easy | medium | hard>"
        std::string difficulty;
    };
    struct StopPlay {};  // "play.stop"
    using PlaySetting = std::
        variant<SetPlayApp, SetPlayFaction, SetPlayColor, SetPlayAiCount, SetPlayStart, SetPlayDifficulty, StopPlay>;

    // Edits of the open map and the editing state (MapEditor::run).
    // Direct edits (scripts and checks): "map.place\t<template>\t<x>\t<y>\t<angle>\t<team>",
    // "map.move\t<id>\t<x>\t<y>", "map.rotate\t<id>\t<angle>", "map.delete\t<id>" (radians).
    struct PlaceObject {
        std::string templateName;
        float x = 0, y = 0, angle = 0;
        std::string team;
    };
    struct MoveObject {
        std::int64_t id = 0;
        float x = 0, y = 0;
    };
    struct RotateObject {
        std::int64_t id = 0;
        float angle = 0;
    };
    struct DeleteObject {
        std::int64_t id = 0;
    };
    struct Undo {};  // "map.undo"
    struct Redo {};  // "map.redo"
    // Input as the client sees it: <x> <y> the ground point under the cursor (both empty off the
    // map), hits the objects under it, nearest first ("1,5,3").
    struct Modifiers {  // "shift,alt,cmd"
        bool shift = false, alt = false, cmd = false;
    };
    struct Press {  // "map.press\t<x>\t<y>\t<modifiers>\t<hits>"
        std::optional<Point> at;
        Modifiers modifiers;
        Ids hits;
    };
    struct Drag {  // "map.drag\t<x>\t<y>"
        std::optional<Point> at;
    };
    struct Release {  // "map.release\t<ids inside the rubber band>"
        Ids band;
    };
    struct RightClick {  // "map.click_right\t<x>\t<y>\t<hits>"
        std::optional<Point> at;
        Ids hits;
    };
    struct DeleteSelection {};  // "edit.delete"
    struct RotateSelection {  // "edit.rotate\t<degrees>"
        double degrees = 0;
    };
    struct Copy {};  // "edit.copy"
    struct Paste {  // "edit.paste[\t<x>\t<y>]": none, the middle of the map
        std::optional<Point> at;
    };
    struct Duplicate {};  // "edit.duplicate"
    struct Cancel {};  // "edit.cancel"
    struct SelectAll {};  // "edit.select_all"
    // The placement tool (these need no open map).
    struct UseObjectTool {};  // "placement.object"
    struct ChooseTemplate {  // "placement.template\t<template>"
        std::string templateName;
    };
    struct UseWaypointTool {};  // "placement.waypoint"
    struct UseRoadTool {  // "placement.road[\t<type>]", "placement.bridge[\t<type>]": "" the last or first
        bool bridge = false;
        std::string type;
    };
    struct SetPlacementOwner {  // "placement.owner\t<team>"
        std::string team;
    };
    struct SetPlacementAngle {  // "placement.angle\t<degrees>"
        double degrees = 0;
    };
    struct ShowPicker {};  // "placement.pick"
    struct CancelPlacement {};  // "placement.cancel"
    struct ClearNote {};  // "note.clear"
    // The inspector: the whole selection, one undo step.
    struct SetSelection {  // "selection.set\t<ids>"
        Ids ids;
    };
    struct SetOwner {  // "selection.owner\t<team>"
        std::string team;
    };
    struct SetHeight {  // "selection.height\t<z>"
        float z = 0;
    };
    struct SetAngle {  // "selection.angle\t<degrees>"
        double degrees = 0;
    };
    struct SetProperty {  // "selection.property\t<key>\t<bool | int | real | string | unicode>\t<value>"
        DictValue value;
    };
    struct UnsetProperty {  // "selection.unset\t<key>"
        std::string key;
    };
    struct LinkWaypoints {};  // "waypoint.link"
    using Edit = std::variant<PlaceObject,
                              MoveObject,
                              RotateObject,
                              DeleteObject,
                              Undo,
                              Redo,
                              Press,
                              Drag,
                              Release,
                              RightClick,
                              DeleteSelection,
                              RotateSelection,
                              Copy,
                              Paste,
                              Duplicate,
                              Cancel,
                              SelectAll,
                              UseObjectTool,
                              ChooseTemplate,
                              UseWaypointTool,
                              UseRoadTool,
                              SetPlacementOwner,
                              SetPlacementAngle,
                              ShowPicker,
                              CancelPlacement,
                              ClearNote,
                              SetSelection,
                              SetOwner,
                              SetHeight,
                              SetAngle,
                              SetProperty,
                              UnsetProperty,
                              LinkWaypoints>;
    // False for the placement tool and note.clear, which work with no map open.
    bool needsMap(const Edit& edit);

    using Command = std::variant<OpenGame,
                                 OpenModel,
                                 PlayAnimation,
                                 OpenMap,
                                 SetUserMaps,
                                 SaveMap,
                                 SaveMapAs,
                                 PlayMap,
                                 PlaySetting,
                                 Edit>;

    // A command that cannot be read: the code send() returns ("error:missing_argument",
    // "error:bad_argument", "error:unknown_command") and, for the map editor's commands, the note
    // the user sees ("" none).
    struct Error {
        std::string code;
        std::string note;
        bool operator==(const Error&) const = default;
    };

    // Reads one command: its name, then its arguments, all checked.
    std::variant<Command, Error> parse(std::string_view text);

    // A number of a command: the whole text in the C locale's format ("-12.5", "1e-05"), finite.
    std::optional<double> parseNumber(std::string_view text);
}
