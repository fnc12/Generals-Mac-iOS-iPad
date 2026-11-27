#pragma once

// The SQLite view-model store the client reads. The core is its only writer.

#include <sqlite_orm/sqlite_orm.h>

#include <cstdint>
#include <string>
#include <vector>

namespace end_editor {

    struct StatusRow {
        int id = 1;
        std::string state;  // "idle" | "loading" | "ready" | "error"
        std::string message;
        std::string gameDir;
        std::int64_t objectCount = 0;
        std::int64_t revision = 0;  // grows on every change
        std::int64_t mapListRevision = 0;  // grows when the maps table is listed again
    };

    struct ObjectRow {
        std::int64_t id = 0;
        std::string name;
        std::string definition;
        std::string parent;
        std::string side;
        std::string kindOf;
        std::string buildable;
        std::string editorSorting;
        std::string displayName;
        std::string model;
        std::string muzzleFlashes;
        std::string hiddenSubObjects;
        std::string sourceFile;
        int line = 0;
    };

    // Problems found while reading the game data (bad archives, unclosed blocks).
    struct WarningRow {
        std::int64_t id = 0;
        std::string text;
    };

    // The model shown in the preview. One row (id 1). `pose` holds the bone matrices of every
    // frame of the current animation (frame_count * bone_count column-major 4x4 floats) and
    // `visible` one byte per frame and bone; without an animation it is the bind pose (1 frame).
    struct ModelRow {
        int id = 1;
        std::string name;
        std::string state;  // "idle" | "loading" | "ready" | "error"
        std::string message;
        std::int64_t revision = 0;
        int boneCount = 0;
        std::string animation;
        int frameCount = 0;
        int frameRate = 30;
        std::vector<char> pose;
        std::vector<char> visible;
        double minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    };

    // Part of the model drawn with one texture. `vertices`: 36-byte records (position xyz,
    // normal xyz, uv, bone index as uint32); `indices`: uint32 triangle list.
    struct ModelPartRow {
        std::int64_t id = 0;
        std::string mesh;
        std::string texture;
        int bone = 0;
        bool skinned = false;
        bool twoSided = false;
        bool hidden = false;
        int blend = 0;  // 0 opaque, 1 alpha test, 2 alpha blend, 3 additive, 4 multiply
        int houseColor = 0;  // 0 none, 1 texture alpha marks player color, 2 whole part tinted
        std::vector<char> vertices;
        std::vector<char> indices;
    };

    struct ModelBoneRow {
        std::int64_t id = 0;
        int index = 0;
        std::string name;
        int parent = -1;
    };

    // RGBA8, rows top to bottom.
    struct ModelTextureRow {
        std::int64_t id = 0;
        std::string name;
        int width = 0;
        int height = 0;
        std::vector<char> rgba;
    };

    struct ModelAnimationRow {
        std::int64_t id = 0;
        std::string name;
        int frameCount = 0;
        int frameRate = 30;
    };

    // Maps the game folders hold (archives and loose Maps folders). `path` is what map.open takes.
    struct MapListRow {
        std::int64_t id = 0;
        std::string path;  // normalized ("maps/alpine assault/alpine assault.map")
        std::string name;  // folder name as written ("Alpine Assault")
        std::string source;  // archive file name or loose file path
        std::int64_t size = 0;
    };

    // The open map. One row (id 1). `heights`: width * height bytes, rows south to north (world
    // height = byte * 0.625, a cell is 10 units, vertex (i, j) is at ((i - border) * 10,
    // (j - border) * 10)). `lighting`: 18 floats: terrain ambient, diffuse, direction, then the
    // same for objects. Models load after the map is "ready"; model_revision grows as they come.
    struct MapRow {
        int id = 1;
        std::string path;
        std::string name;
        std::string state;  // "idle" | "loading" | "ready" | "error"
        std::string message;
        std::int64_t revision = 0;
        int width = 0;
        int height = 0;
        int border = 0;
        double playableX0 = 0, playableY0 = 0, playableX1 = 0, playableY1 = 0;
        int timeOfDay = 2;
        std::vector<char> lighting;
        std::vector<char> heights;
        int objectCount = 0;
        int modelCount = 0;
        int modelsLoaded = 0;
        std::int64_t modelRevision = 0;
        double waterR = 1, waterG = 1, waterB = 1, waterA = 0.5;
        double waterRepeat = 32;
        // Editing: object_revision grows whenever map_objects changes after the map opened;
        // last_object is the object the last edit, undo or redo changed; edited: changes since
        // opening or saving. edit_message: the last edit or save problem or result.
        std::int64_t objectRevision = 0;
        int undoCount = 0;
        int redoCount = 0;
        bool edited = false;
        std::int64_t lastObject = 0;
        std::string editMessage;
        std::string savedPath;
        // map.save_as found another map at its target: the path, until the next save or map change.
        std::string saveConflict;
    };

    // Players of the open map's SidesList: what a placed object can be owned by (its team).
    struct MapPlayerRow {
        std::int64_t id = 0;
        std::string name;  // "" for the neutral player
        std::string displayName;
        std::string team;  // the player's default team, what originalOwner names
        int color = 0xFFFFFF;
        bool neutral = false;
        // A start position (1..: whoever starts there in the game owns the object), 0 a player of
        // the map's SidesList (map_document.h, MapPlayer::start).
        int start = 0;
    };

    // Textures of the map view: "terrain" (the atlas of all texture classes) and "water". RGBA8,
    // rows top to bottom.
    struct MapTextureRow {
        std::int64_t id = 0;
        std::string name;
        int width = 0;
        int height = 0;
        std::vector<char> rgba;
    };

    // A square of terrain cells (x, y: first cell). `vertices`: 56-byte records (position xyz,
    // normal xyz, base uv, blend uv, extra uv, blend and extra weights); `indices`: uint32.
    struct MapTerrainRow {
        std::int64_t id = 0;
        int x = 0;
        int y = 0;
        std::vector<char> vertices;
        std::vector<char> indices;
    };

    // A water area or river: `vertices` float x y z (world units), `indices` uint32 triangles.
    struct MapWaterRow {
        std::int64_t id = 0;
        std::string name;
        bool river = false;
        double height = 0;
        std::vector<char> vertices;
        std::vector<char> indices;
    };

    // A placed object. kind: "model", "marker", "waypoint", "road", "bridge". z is the world
    // height (ground + height), height the object's height above the ground. model_id: map_models
    // row, 0 when drawn as a marker. color: owner's player color 0xRRGGBB.
    struct MapObjectRow {
        std::int64_t id = 0;
        std::string templateName;
        std::string kind;
        double x = 0, y = 0, z = 0;
        double height = 0;
        double angle = 0;
        int flags = 0;
        std::int64_t modelId = 0;
        std::string owner;
        int color = 0xFFFFFF;  // the owner's color; a start waypoint's, its start position's
        int start = 0;  // a start waypoint ("Player_<n>_Start"): n, else 0
    };

    // The object's properties dictionary as the map stores it.
    struct MapPropertyRow {
        std::int64_t id = 0;
        std::int64_t objectId = 0;
        std::string key;
        std::string type;  // "bool" | "int" | "real" | "string" | "unicode"
        std::string value;
    };

    // A model used by the map's objects; state "pending" until it is loaded.
    struct MapModelRow {
        std::int64_t id = 0;
        std::string name;
        std::string state;  // "pending" | "ready" | "error"
        std::string message;
        double minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    };

    // A part of a map model in its bind pose (vertices already moved by their bones: 36-byte
    // records like model_parts, bone always 0).
    struct MapModelPartRow {
        std::int64_t id = 0;
        std::int64_t modelId = 0;
        std::string texture;
        int blend = 0;
        int houseColor = 0;
        bool twoSided = false;
        std::vector<char> vertices;
        std::vector<char> indices;
    };

    // Textures of the map models, by name (shared between models), at most 512 px a side.
    struct MapModelTextureRow {
        std::int64_t id = 0;
        std::string name;
        int width = 0;
        int height = 0;
        std::vector<char> rgba;
    };

    struct MapWarningRow {
        std::int64_t id = 0;
        std::string text;
    };

    // What the user is doing with the map (map_editor.h), one row (id 1); revision grows on every
    // change. tool: "object" | "waypoint" | "road" | "bridge"; template_name: the object a right
    // click places ("" none chosen); road_type: the road tools' Roads.ini name; team: owner of
    // placed objects; angle: degrees. picker: the client shows the object picker (a right click
    // with no object chosen, or placement.pick); pending: that right click's ground point, placed
    // once an object is chosen. road_start: the first point of a road or bridge. gesture: the
    // mouse press in progress: "press_pan", "press_move", "press_band", "press_orbit" (not moved
    // yet), "pan", "orbit" (the client moves its camera), "move" (the selection is dragged by
    // drag_dx, drag_dy: the client draws it there), "band" (the client draws its rectangle) or
    // "". note: what the user is told ("info" or "error"); it stays until the next note or the
    // next change of the map; note_serial grows with each. selection_revision grows when
    // map_selection changes.
    struct MapEditorRow {
        int id = 1;
        std::string tool = "object";
        std::string templateName;
        std::string roadType;
        std::string team;
        double angle = 0;
        bool picker = false;
        bool pending = false;
        double pendingX = 0, pendingY = 0;
        bool roadStart = false;
        double roadX = 0, roadY = 0;
        std::string gesture;
        double dragX = 0, dragY = 0;
        std::string note;
        std::string noteKind;
        std::int64_t noteSerial = 0;
        std::int64_t selectionRevision = 0;
        int selectionCount = 0;
        int clipboardCount = 0;
        std::int64_t revision = 0;
    };

    // The selected objects, in the order they were selected (id).
    struct MapSelectionRow {
        std::int64_t id = 0;
        std::int64_t objectId = 0;
    };

    // Lines the map view draws between objects: "waypoint" (a link of WaypointsList, from -> to),
    // "road" and "bridge" (a segment's two points).
    struct MapLinkRow {
        std::int64_t id = 0;
        std::string kind;
        std::int64_t fromId = 0;
        std::int64_t toId = 0;
    };

    // Road and bridge types of Roads.ini, what placement.road / placement.bridge take.
    struct RoadTypeRow {
        std::int64_t id = 0;
        std::string name;
        bool bridge = false;
    };

    // Playable factions of PlayerTemplate.ini (PlayableSide = Yes), what play.faction takes.
    struct FactionRow {
        std::int64_t id = 0;
        std::string name;  // e.g. "FactionAmerica"
        std::string side;  // e.g. "America"
    };

    // The player colors of Multiplayer.ini (MultiplayerColor), what play.color takes. `index` is
    // the game's color index (-testMapColor): the entries in the order the game reads them.
    struct PlayerColorRow {
        std::int64_t id = 0;
        int index = 0;
        std::string name;  // e.g. "ColorBlue"
        int rgb = 0xFFFFFF;  // RGBColor, 0xRRGGBB
    };

    // The Play button (map.play): its settings, kept across launches of the editor, and the
    // last test run. One row (id 1).
    struct PlayRow {
        int id = 1;
        std::string app;  // the game app (or executable) as chosen; "" uses the default
        std::string appResolved;  // what map.play starts: the chosen app or the default one
        std::string faction = "FactionAmerica";  // the local player's faction, or "Random"
        std::string color = "Random";  // the local player's color (a player_colors name), or "Random"
        int aiCount = 1;  // AI opponents (the game caps it at the map's start positions - 1)
        std::string aiDifficulty = "medium";  // "easy" | "medium" | "hard"
        int startPosition = 1;  // 1-based, 0 random
        // "idle" | "running" (the game started and runs) | "exited" (exit_code) | "error" (message)
        std::string state = "idle";
        std::string message;
        std::string mapPath;  // the test copy of the map the game was started on
        std::string command;  // what was started: the program and its arguments, tab separated
        std::string logPath;  // the game's output
        std::int64_t pid = 0;
        int exitCode = 0;
        std::int64_t runs = 0;  // grows with every map.play that started the game
        std::int64_t revision = 0;
    };

    // The core's connection writes; a reader (the cli, a client) opens the same store with
    // {.open_forever = true, .open_mode = sqlite_orm::db_open_mode::readonly}.
    inline auto makeStorage(const std::string& path, sqlite_orm::connection_control connection = {}) {
        using namespace sqlite_orm;
        return make_storage(path,
                            std::move(connection),
                            make_index("objects_name", &ObjectRow::name),
                            make_index("map_object_properties_object", &MapPropertyRow::objectId),
                            make_index("map_model_parts_model", &MapModelPartRow::modelId),
                            make_table("status",
                                       make_column("id", &StatusRow::id, primary_key()),
                                       make_column("state", &StatusRow::state),
                                       make_column("message", &StatusRow::message),
                                       make_column("game_dir", &StatusRow::gameDir),
                                       make_column("object_count", &StatusRow::objectCount),
                                       make_column("revision", &StatusRow::revision),
                                       make_column("map_list_revision", &StatusRow::mapListRevision)),
                            make_table("objects",
                                       make_column("id", &ObjectRow::id, primary_key().autoincrement()),
                                       make_column("name", &ObjectRow::name),
                                       make_column("definition", &ObjectRow::definition),
                                       make_column("parent", &ObjectRow::parent),
                                       make_column("side", &ObjectRow::side),
                                       make_column("kind_of", &ObjectRow::kindOf),
                                       make_column("buildable", &ObjectRow::buildable),
                                       make_column("editor_sorting", &ObjectRow::editorSorting),
                                       make_column("display_name", &ObjectRow::displayName),
                                       make_column("model", &ObjectRow::model),
                                       make_column("muzzle_flashes", &ObjectRow::muzzleFlashes),
                                       make_column("hidden_sub_objects", &ObjectRow::hiddenSubObjects),
                                       make_column("source_file", &ObjectRow::sourceFile),
                                       make_column("line", &ObjectRow::line)),
                            make_table("warnings",
                                       make_column("id", &WarningRow::id, primary_key().autoincrement()),
                                       make_column("text", &WarningRow::text)),
                            make_table("model",
                                       make_column("id", &ModelRow::id, primary_key()),
                                       make_column("name", &ModelRow::name),
                                       make_column("state", &ModelRow::state),
                                       make_column("message", &ModelRow::message),
                                       make_column("revision", &ModelRow::revision),
                                       make_column("bone_count", &ModelRow::boneCount),
                                       make_column("animation", &ModelRow::animation),
                                       make_column("frame_count", &ModelRow::frameCount),
                                       make_column("frame_rate", &ModelRow::frameRate),
                                       make_column("pose", &ModelRow::pose),
                                       make_column("visible", &ModelRow::visible),
                                       make_column("min_x", &ModelRow::minX),
                                       make_column("min_y", &ModelRow::minY),
                                       make_column("min_z", &ModelRow::minZ),
                                       make_column("max_x", &ModelRow::maxX),
                                       make_column("max_y", &ModelRow::maxY),
                                       make_column("max_z", &ModelRow::maxZ)),
                            make_table("model_parts",
                                       make_column("id", &ModelPartRow::id, primary_key().autoincrement()),
                                       make_column("mesh", &ModelPartRow::mesh),
                                       make_column("texture", &ModelPartRow::texture),
                                       make_column("bone", &ModelPartRow::bone),
                                       make_column("skinned", &ModelPartRow::skinned),
                                       make_column("two_sided", &ModelPartRow::twoSided),
                                       make_column("hidden", &ModelPartRow::hidden),
                                       make_column("blend", &ModelPartRow::blend),
                                       make_column("house_color", &ModelPartRow::houseColor),
                                       make_column("vertices", &ModelPartRow::vertices),
                                       make_column("indices", &ModelPartRow::indices)),
                            make_table("model_bones",
                                       make_column("id", &ModelBoneRow::id, primary_key().autoincrement()),
                                       make_column("idx", &ModelBoneRow::index),
                                       make_column("name", &ModelBoneRow::name),
                                       make_column("parent", &ModelBoneRow::parent)),
                            make_table("model_textures",
                                       make_column("id", &ModelTextureRow::id, primary_key().autoincrement()),
                                       make_column("name", &ModelTextureRow::name),
                                       make_column("width", &ModelTextureRow::width),
                                       make_column("height", &ModelTextureRow::height),
                                       make_column("rgba", &ModelTextureRow::rgba)),
                            make_table("model_animations",
                                       make_column("id", &ModelAnimationRow::id, primary_key().autoincrement()),
                                       make_column("name", &ModelAnimationRow::name),
                                       make_column("frame_count", &ModelAnimationRow::frameCount),
                                       make_column("frame_rate", &ModelAnimationRow::frameRate)),
                            make_table("maps",
                                       make_column("id", &MapListRow::id, primary_key().autoincrement()),
                                       make_column("path", &MapListRow::path),
                                       make_column("name", &MapListRow::name),
                                       make_column("source", &MapListRow::source),
                                       make_column("size", &MapListRow::size)),
                            make_table("map",
                                       make_column("id", &MapRow::id, primary_key()),
                                       make_column("path", &MapRow::path),
                                       make_column("name", &MapRow::name),
                                       make_column("state", &MapRow::state),
                                       make_column("message", &MapRow::message),
                                       make_column("revision", &MapRow::revision),
                                       make_column("width", &MapRow::width),
                                       make_column("height", &MapRow::height),
                                       make_column("border", &MapRow::border),
                                       make_column("playable_x0", &MapRow::playableX0),
                                       make_column("playable_y0", &MapRow::playableY0),
                                       make_column("playable_x1", &MapRow::playableX1),
                                       make_column("playable_y1", &MapRow::playableY1),
                                       make_column("time_of_day", &MapRow::timeOfDay),
                                       make_column("lighting", &MapRow::lighting),
                                       make_column("heights", &MapRow::heights),
                                       make_column("object_count", &MapRow::objectCount),
                                       make_column("model_count", &MapRow::modelCount),
                                       make_column("models_loaded", &MapRow::modelsLoaded),
                                       make_column("model_revision", &MapRow::modelRevision),
                                       make_column("water_r", &MapRow::waterR),
                                       make_column("water_g", &MapRow::waterG),
                                       make_column("water_b", &MapRow::waterB),
                                       make_column("water_a", &MapRow::waterA),
                                       make_column("water_repeat", &MapRow::waterRepeat),
                                       make_column("object_revision", &MapRow::objectRevision),
                                       make_column("undo_count", &MapRow::undoCount),
                                       make_column("redo_count", &MapRow::redoCount),
                                       make_column("edited", &MapRow::edited),
                                       make_column("last_object", &MapRow::lastObject),
                                       make_column("edit_message", &MapRow::editMessage),
                                       make_column("saved_path", &MapRow::savedPath),
                                       make_column("save_conflict", &MapRow::saveConflict)),
                            make_table("map_players",
                                       make_column("id", &MapPlayerRow::id, primary_key().autoincrement()),
                                       make_column("name", &MapPlayerRow::name),
                                       make_column("display_name", &MapPlayerRow::displayName),
                                       make_column("team", &MapPlayerRow::team),
                                       make_column("color", &MapPlayerRow::color),
                                       make_column("neutral", &MapPlayerRow::neutral),
                                       make_column("start", &MapPlayerRow::start)),
                            make_table("map_textures",
                                       make_column("id", &MapTextureRow::id, primary_key().autoincrement()),
                                       make_column("name", &MapTextureRow::name),
                                       make_column("width", &MapTextureRow::width),
                                       make_column("height", &MapTextureRow::height),
                                       make_column("rgba", &MapTextureRow::rgba)),
                            make_table("map_terrain",
                                       make_column("id", &MapTerrainRow::id, primary_key().autoincrement()),
                                       make_column("x", &MapTerrainRow::x),
                                       make_column("y", &MapTerrainRow::y),
                                       make_column("vertices", &MapTerrainRow::vertices),
                                       make_column("indices", &MapTerrainRow::indices)),
                            make_table("map_water",
                                       make_column("id", &MapWaterRow::id, primary_key().autoincrement()),
                                       make_column("name", &MapWaterRow::name),
                                       make_column("river", &MapWaterRow::river),
                                       make_column("height", &MapWaterRow::height),
                                       make_column("vertices", &MapWaterRow::vertices),
                                       make_column("indices", &MapWaterRow::indices)),
                            make_table("map_objects",
                                       make_column("id", &MapObjectRow::id, primary_key()),
                                       make_column("template_name", &MapObjectRow::templateName),
                                       make_column("kind", &MapObjectRow::kind),
                                       make_column("x", &MapObjectRow::x),
                                       make_column("y", &MapObjectRow::y),
                                       make_column("z", &MapObjectRow::z),
                                       make_column("height", &MapObjectRow::height),
                                       make_column("angle", &MapObjectRow::angle),
                                       make_column("flags", &MapObjectRow::flags),
                                       make_column("model_id", &MapObjectRow::modelId),
                                       make_column("owner", &MapObjectRow::owner),
                                       make_column("color", &MapObjectRow::color),
                                       make_column("start", &MapObjectRow::start)),
                            make_table("map_object_properties",
                                       make_column("id", &MapPropertyRow::id, primary_key().autoincrement()),
                                       make_column("object_id", &MapPropertyRow::objectId),
                                       make_column("key", &MapPropertyRow::key),
                                       make_column("type", &MapPropertyRow::type),
                                       make_column("value", &MapPropertyRow::value)),
                            make_table("map_models",
                                       make_column("id", &MapModelRow::id, primary_key()),
                                       make_column("name", &MapModelRow::name),
                                       make_column("state", &MapModelRow::state),
                                       make_column("message", &MapModelRow::message),
                                       make_column("min_x", &MapModelRow::minX),
                                       make_column("min_y", &MapModelRow::minY),
                                       make_column("min_z", &MapModelRow::minZ),
                                       make_column("max_x", &MapModelRow::maxX),
                                       make_column("max_y", &MapModelRow::maxY),
                                       make_column("max_z", &MapModelRow::maxZ)),
                            make_table("map_model_parts",
                                       make_column("id", &MapModelPartRow::id, primary_key().autoincrement()),
                                       make_column("model_id", &MapModelPartRow::modelId),
                                       make_column("texture", &MapModelPartRow::texture),
                                       make_column("blend", &MapModelPartRow::blend),
                                       make_column("house_color", &MapModelPartRow::houseColor),
                                       make_column("two_sided", &MapModelPartRow::twoSided),
                                       make_column("vertices", &MapModelPartRow::vertices),
                                       make_column("indices", &MapModelPartRow::indices)),
                            make_table("map_model_textures",
                                       make_column("id", &MapModelTextureRow::id, primary_key().autoincrement()),
                                       make_column("name", &MapModelTextureRow::name),
                                       make_column("width", &MapModelTextureRow::width),
                                       make_column("height", &MapModelTextureRow::height),
                                       make_column("rgba", &MapModelTextureRow::rgba)),
                            make_table("map_warnings",
                                       make_column("id", &MapWarningRow::id, primary_key().autoincrement()),
                                       make_column("text", &MapWarningRow::text)),
                            make_table("map_editor",
                                       make_column("id", &MapEditorRow::id, primary_key()),
                                       make_column("tool", &MapEditorRow::tool),
                                       make_column("template_name", &MapEditorRow::templateName),
                                       make_column("road_type", &MapEditorRow::roadType),
                                       make_column("team", &MapEditorRow::team),
                                       make_column("angle", &MapEditorRow::angle),
                                       make_column("picker", &MapEditorRow::picker),
                                       make_column("pending", &MapEditorRow::pending),
                                       make_column("pending_x", &MapEditorRow::pendingX),
                                       make_column("pending_y", &MapEditorRow::pendingY),
                                       make_column("road_start", &MapEditorRow::roadStart),
                                       make_column("road_x", &MapEditorRow::roadX),
                                       make_column("road_y", &MapEditorRow::roadY),
                                       make_column("gesture", &MapEditorRow::gesture),
                                       make_column("drag_dx", &MapEditorRow::dragX),
                                       make_column("drag_dy", &MapEditorRow::dragY),
                                       make_column("note", &MapEditorRow::note),
                                       make_column("note_kind", &MapEditorRow::noteKind),
                                       make_column("note_serial", &MapEditorRow::noteSerial),
                                       make_column("selection_revision", &MapEditorRow::selectionRevision),
                                       make_column("selection_count", &MapEditorRow::selectionCount),
                                       make_column("clipboard_count", &MapEditorRow::clipboardCount),
                                       make_column("revision", &MapEditorRow::revision)),
                            make_table("map_selection",
                                       make_column("id", &MapSelectionRow::id, primary_key().autoincrement()),
                                       make_column("object_id", &MapSelectionRow::objectId)),
                            make_table("map_links",
                                       make_column("id", &MapLinkRow::id, primary_key().autoincrement()),
                                       make_column("kind", &MapLinkRow::kind),
                                       make_column("from_id", &MapLinkRow::fromId),
                                       make_column("to_id", &MapLinkRow::toId)),
                            make_table("road_types",
                                       make_column("id", &RoadTypeRow::id, primary_key().autoincrement()),
                                       make_column("name", &RoadTypeRow::name),
                                       make_column("bridge", &RoadTypeRow::bridge)),
                            make_table("factions",
                                       make_column("id", &FactionRow::id, primary_key().autoincrement()),
                                       make_column("name", &FactionRow::name),
                                       make_column("side", &FactionRow::side)),
                            make_table("player_colors",
                                       make_column("id", &PlayerColorRow::id, primary_key().autoincrement()),
                                       make_column("idx", &PlayerColorRow::index),
                                       make_column("name", &PlayerColorRow::name),
                                       make_column("rgb", &PlayerColorRow::rgb)),
                            make_table("play",
                                       make_column("id", &PlayRow::id, primary_key()),
                                       make_column("app", &PlayRow::app),
                                       make_column("app_resolved", &PlayRow::appResolved),
                                       make_column("faction", &PlayRow::faction),
                                       make_column("color", &PlayRow::color),
                                       make_column("ai_count", &PlayRow::aiCount),
                                       make_column("ai_difficulty", &PlayRow::aiDifficulty),
                                       make_column("start_position", &PlayRow::startPosition),
                                       make_column("state", &PlayRow::state),
                                       make_column("message", &PlayRow::message),
                                       make_column("map_path", &PlayRow::mapPath),
                                       make_column("command", &PlayRow::command),
                                       make_column("log_path", &PlayRow::logPath),
                                       make_column("pid", &PlayRow::pid),
                                       make_column("exit_code", &PlayRow::exitCode),
                                       make_column("runs", &PlayRow::runs),
                                       make_column("revision", &PlayRow::revision)));
    }

    using Storage = decltype(makeStorage(""));
}
