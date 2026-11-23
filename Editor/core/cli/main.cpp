// Headless harness: drives the core through the C ABI, the way a client does, and prints what
// the store holds.
//   end-editor-cli <game folder> [--base <base Generals folder>] [--db <path>]
//                  [--model <name> | --object <object name>] [--anim <animation>] [--all-models]
// --object opens the object's model with what its Draw module hides when idle, like the app.
// --all-models opens the model of every object and prints a summary of what failed.
#include "end_editor/editor_core_c.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
    sqlite3 *db = nullptr;

    void print(const char *sql) {
        sqlite3_stmt *statement = nullptr;
        sqlite3_prepare_v2(db, sql, -1, &statement, nullptr);
        while(sqlite3_step(statement) == SQLITE_ROW) {
            const int columns = sqlite3_column_count(statement);
            for(int i = 0; i < columns; ++i) {
                const auto *text = sqlite3_column_text(statement, i);
                std::printf("%s%s", i ? " | " : "", text ? reinterpret_cast<const char *>(text) : "");
            }
            std::printf("\n");
        }
        sqlite3_finalize(statement);
    }

    std::vector<std::string> column(const char *sql) {
        std::vector<std::string> result;
        sqlite3_stmt *statement = nullptr;
        sqlite3_prepare_v2(db, sql, -1, &statement, nullptr);
        while(sqlite3_step(statement) == SQLITE_ROW) {
            const auto *text = sqlite3_column_text(statement, 0);
            result.emplace_back(text ? reinterpret_cast<const char *>(text) : "");
        }
        sqlite3_finalize(statement);
        return result;
    }

    bool send(const std::string &command) {
        const std::string result = end_editor_send(command.c_str());
        if(!result.empty()) {
            std::fprintf(stderr, "%s: %s\n", command.c_str(), result.c_str());
            return false;
        }
        end_editor_update();
        return true;
    }
}

int main(int argc, char **argv) {
    std::string gameDir, baseDir, dbArg, modelName, objectName, animation;
    bool allModels = false;
    for(int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if(arg == "--base") {
            baseDir = next();
        } else if(arg == "--db") {
            dbArg = next();
        } else if(arg == "--model") {
            modelName = next();
        } else if(arg == "--object") {
            objectName = next();
        } else if(arg == "--anim") {
            animation = next();
        } else if(arg == "--all-models") {
            allModels = true;
        } else {
            gameDir = arg;
        }
    }
    if(gameDir.empty()) {
        std::fprintf(stderr, "usage: %s <game folder> [--base <folder>] [--db <path>] [--model <name> [--anim <a>]] "
                             "[--all-models]\n", argv[0]);
        return 2;
    }
    if(!end_editor_initialize(dbArg.empty() ? nullptr : dbArg.c_str())) {
        std::fprintf(stderr, "cannot open the store\n");
        return 1;
    }
    const std::string dbPath = end_editor_db_path();
    // Read the store like a client would, with its own connection.
    if(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "cannot read %s\n", dbPath.c_str());
        return 1;
    }

    if(!send("game.open\t" + gameDir + (baseDir.empty() ? "" : "\t" + baseDir))) {
        return 1;
    }
    std::printf("store: %s\n", dbPath.c_str());
    print("SELECT state, message FROM status");
    std::printf("-- objects per side\n");
    print("SELECT side, COUNT(*) FROM objects GROUP BY side ORDER BY COUNT(*) DESC LIMIT 10");
    std::printf("-- warnings (first 20)\n");
    print("SELECT text FROM warnings LIMIT 20");

    std::string openCommand = modelName.empty() ? "" : "model.open\t" + modelName;
    if(!objectName.empty()) {
        const auto row = column(("SELECT model || char(9) || muzzle_flashes || char(9) || hidden_sub_objects FROM objects "
                                 "WHERE name = '" + objectName + "'").c_str());
        if(row.empty()) {
            std::fprintf(stderr, "no object %s\n", objectName.c_str());
            return 1;
        }
        openCommand = "model.open\t" + row.front();
        std::printf("-- object %s: %s\n", objectName.c_str(), row.front().c_str());
    }
    if(!openCommand.empty()) {
        send(openCommand);
        if(!animation.empty()) {
            send("model.animation\t" + animation);
        }
        std::printf("-- model\n");
        print("SELECT name, state, message, bone_count, animation, frame_count, frame_rate, length(pose), "
              "min_x, min_y, min_z, max_x, max_y, max_z FROM model");
        std::printf("-- parts\n");
        print("SELECT mesh, texture, bone, skinned, hidden, blend, house_color, length(vertices) / 36, length(indices) / 12 "
              "FROM model_parts");
        std::printf("-- textures\n");
        print("SELECT name, width, height FROM model_textures");
        std::printf("-- animations\n");
        print("SELECT name, frame_count, frame_rate FROM model_animations");
    }

    if(allModels) {
        const auto models = column("SELECT DISTINCT model FROM objects WHERE model <> '' ORDER BY model");
        std::map<std::string, int> errors;
        std::vector<std::string> failed;
        int ready = 0, withAnimations = 0, untextured = 0;
        for(const auto &model: models) {
            send("model.open\t" + model);
            const auto state = column("SELECT state || '|' || message FROM model").front();
            if(state.rfind("ready", 0) == 0) {
                ++ready;
                withAnimations += column("SELECT COUNT(*) FROM model_animations").front() != "0";
                untextured += column("SELECT COUNT(*) FROM model_textures").front() == "0";
            } else {
                const auto message = state.substr(state.find('|') + 1);
                ++errors[message.substr(0, message.find(' ', message.find(' ') + 1))];
                failed.push_back(model);
            }
        }
        std::printf("-- all models: %zu, ready %d (with animations %d, without textures %d)\n", models.size(), ready,
                    withAnimations, untextured);
        for(const auto &[kind, count]: errors) {
            std::printf("   %d x %s...\n", count, kind.c_str());
        }
        for(std::size_t i = 0; i < failed.size() && i < 20; ++i) {
            std::printf("   failed: %s\n", failed[i].c_str());
        }
    }
    sqlite3_close(db);
    end_editor_shutdown();
    return 0;
}
