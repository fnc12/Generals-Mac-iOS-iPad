#pragma once

// The SQLite view-model store the client reads. The core is its only writer.

#include <sqlite_orm/sqlite_orm.h>

#include <cstdint>
#include <string>

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
                       make_column("text", &WarningRow::text)));
    }

    using Storage = decltype(makeStorage(""));
}
