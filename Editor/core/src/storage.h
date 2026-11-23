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

    inline auto makeStorage(const std::string &path) {
        using namespace sqlite_orm;
        return make_storage(
            path,
            make_index("objects_name", &ObjectRow::name),
            make_table("status",
                       make_column("id", &StatusRow::id, primary_key()),
                       make_column("state", &StatusRow::state),
                       make_column("message", &StatusRow::message),
                       make_column("game_dir", &StatusRow::gameDir),
                       make_column("object_count", &StatusRow::objectCount),
                       make_column("revision", &StatusRow::revision)),
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
                       make_column("frame_rate", &ModelAnimationRow::frameRate)));
    }

    using Storage = decltype(makeStorage(""));
}
