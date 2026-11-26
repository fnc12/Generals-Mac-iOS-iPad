// Headless harness: drives the core through the C ABI, the way a client does, and prints what
// the store holds.
//   end-editor-cli <game folder> [--base <base Generals folder>] [--db <path>]
//                  [--model <name> | --object <object name>] [--anim <animation>] [--all-models]
//                  [--map <path>] [--all-maps]
// --object opens the object's model with what its Draw module hides when idle, like the app.
// --all-models opens the model of every object and prints a summary of what failed.
// --map opens one map (a path from the maps table or a .map file) and waits for its models.
// --all-maps opens every listed map (without models) and counts errors and warnings.
// --roundtrip-maps <folder> opens every listed map, saves it unchanged into the folder (map.save)
//                  and compares the file with the unpacked original, byte for byte.
// --edit-maps <folder> opens every listed map, moves, turns, deletes and places objects (with an
//                  undo and a redo), saves, opens the saved file and checks what it holds.
#include "end_editor/editor_core_c.h"

#include "compression.h"
#include "game_files.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    bool allModels = false, allMaps = false;
    std::string mapPath, roundTripDir, editDir;
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
        } else if(arg == "--map") {
            mapPath = next();
        } else if(arg == "--all-maps") {
            allMaps = true;
        } else if(arg == "--roundtrip-maps") {
            roundTripDir = next();
        } else if(arg == "--edit-maps") {
            editDir = next();
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
    if(!mapPath.empty()) {
        if(!send("map.open\t" + mapPath)) {
            return 1;
        }
        while(end_editor_pending_work() > 0) {
            end_editor_update();
        }
        std::printf("-- map\n");
        print("SELECT name, state, message, width, height, border, playable_x1, playable_y1, time_of_day, "
              "models_loaded || '/' || model_count FROM map");
        std::printf("-- map warnings\n");
        print("SELECT text FROM map_warnings");
        std::printf("-- objects by kind\n");
        print("SELECT kind, COUNT(*) FROM map_objects GROUP BY kind");
        std::printf("-- models: state, count\n");
        print("SELECT state, COUNT(*) FROM map_models GROUP BY state");
        print("SELECT 'failed: ' || name || ': ' || message FROM map_models WHERE state <> 'ready' LIMIT 20");
        std::printf("-- terrain chunks, vertices, textures\n");
        print("SELECT COUNT(*), SUM(length(vertices)) / 56 FROM map_terrain");
        print("SELECT name, width, height FROM map_textures");
        print("SELECT COUNT(*), SUM(width * height * 4) FROM map_model_textures");
        std::printf("-- water\n");
        print("SELECT name, river, height, length(indices) / 12 FROM map_water");
    }

    if(allMaps) {
        const auto paths = column("SELECT path FROM maps ORDER BY path");
        int ready = 0, clean = 0;
        std::map<std::string, int> warningKinds;
        std::map<std::string, long> kinds;
        long unknownTemplates = 0;
        std::vector<std::string> failed;
        for(const auto &path: paths) {
            send("map.open\t" + path);
            const auto state = column("SELECT state || '|' || message FROM map").front();
            if(state.rfind("ready", 0) == 0) {
                ++ready;
                for(const auto &row: column("SELECT kind || '|' || COUNT(*) FROM map_objects GROUP BY kind")) {
                    kinds[row.substr(0, row.find('|'))] += std::stol(row.substr(row.find('|') + 1));
                }
                unknownTemplates += std::stol(column("SELECT COUNT(*) FROM map_objects WHERE kind = 'marker' AND "
                                                     "template_name NOT IN (SELECT name FROM objects)").front());
                const auto warnings = column("SELECT text FROM map_warnings");
                clean += warnings.empty();
                for(const auto &warning: warnings) {
                    ++warningKinds[warning.substr(0, warning.find(':'))];
                    std::printf("   %s: %s\n", path.c_str(), warning.c_str());
                }
            } else {
                failed.push_back(path + ": " + state);
            }
        }
        std::printf("-- all maps: %zu, ready %d, without warnings %d\n", paths.size(), ready, clean);
        for(const auto &[kind, count]: warningKinds) {
            std::printf("   %d x %s\n", count, kind.c_str());
        }
        std::printf("-- objects of all maps by kind (%ld of the markers have no known template)\n", unknownTemplates);
        for(const auto &[kind, count]: kinds) {
            std::printf("   %s: %ld\n", kind.c_str(), count);
        }
        for(const auto &item: failed) {
            std::printf("   failed: %s\n", item.c_str());
        }
    }
    if(!roundTripDir.empty()) {
        // The original bytes, read like the core reads them.
        end_editor::GameFiles files;
        std::vector<std::string> warnings;
        std::string error;
        std::vector<std::filesystem::path> folders{gameDir};
        if(!baseDir.empty()) {
            folders.emplace_back(baseDir);
        }
        files.mount(folders, warnings, error);
        std::filesystem::create_directories(roundTripDir);
        const auto paths = column("SELECT path FROM maps ORDER BY path");
        int identical = 0;
        for(const auto &path: paths) {
            std::string packed, original;
            if(!files.read(path, packed) || !end_editor::decompressGameData(packed, original, error)) {
                std::printf("   %s: cannot read the original\n", path.c_str());
                continue;
            }
            const auto out = (std::filesystem::path(roundTripDir) / "roundtrip.map").string();
            send("map.open\t" + path);
            send("map.save\t" + out);
            while(column("SELECT saved_path FROM map").front() != out && end_editor_pending_work() > 0 &&
                  column("SELECT edit_message FROM map").front().rfind("error", 0) != 0) {
                end_editor_update();
            }
            std::ifstream file(out, std::ios::binary);
            const std::string saved((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if(saved == original) {
                ++identical;
                continue;
            }
            std::size_t at = 0;
            while(at < saved.size() && at < original.size() && saved[at] == original[at]) {
                ++at;
            }
            std::printf("   %s: differs at byte %zu (saved %zu bytes, original %zu): %s\n", path.c_str(), at,
                        saved.size(), original.size(), column("SELECT edit_message FROM map").front().c_str());
        }
        std::printf("-- round trip: %d of %zu maps saved byte for byte as they were\n", identical, paths.size());
    }

    if(!editDir.empty()) {
        std::filesystem::create_directories(editDir);
        const auto out = (std::filesystem::path(editDir) / "edited.map").string();
        const auto placed = column("SELECT name FROM objects WHERE model <> '' AND kind_of LIKE '%VEHICLE%' "
                                   "ORDER BY name LIMIT 1");
        const auto paths = column("SELECT path FROM maps ORDER BY path");
        int good = 0;
        const auto number = [&](const std::string &sql) {
            const auto values = column(sql.c_str());
            return values.empty() ? 0.0 : std::stod(values.front());
        };
        const auto wait = [&]() {
            while(end_editor_pending_work() > 0 && column("SELECT saved_path FROM map").front() != out &&
                  column("SELECT edit_message FROM map").front().rfind("error", 0) != 0) {
                end_editor_update();
            }
        };
        for(const auto &path: paths) {
            send("map.open\t" + path);
            const int before = int(number("SELECT COUNT(*) FROM map_objects"));
            if(before < 3 || placed.empty()) {
                ++good;  // nothing to edit
                continue;
            }
            const auto team = column("SELECT team FROM map_players ORDER BY neutral DESC, id LIMIT 1");
            // Object 1 moves by (10, 20), object 2 turns to 1 radian, object 3 goes; one object
            // is placed in the middle of the map, a second one is placed, undone and redone, a
            // third is placed and undone.
            const double x1 = number("SELECT x FROM map_objects WHERE id = 1") + 10;
            const double y1 = number("SELECT y FROM map_objects WHERE id = 1") + 20;
            const double cx = number("SELECT (playable_x0 + playable_x1) / 2 FROM map");
            const double cy = number("SELECT (playable_y0 + playable_y1) / 2 FROM map");
            const auto at = [](double v) { return std::to_string(v); };
            const std::string owner = team.empty() ? "" : team.front();
            for(const auto &command: std::vector<std::string>{
                    "map.move\t1\t" + at(x1) + "\t" + at(y1), "map.rotate\t2\t1", "map.delete\t3",
                    "map.place\t" + placed.front() + "\t" + at(cx) + "\t" + at(cy) + "\t0.5\t" + owner,
                    "map.place\t" + placed.front() + "\t" + at(cx + 50) + "\t" + at(cy) + "\t0\t" + owner,
                    "map.undo", "map.redo",
                    "map.place\t" + placed.front() + "\t" + at(cx + 100) + "\t" + at(cy) + "\t0\t" + owner,
                    "map.undo", "map.save\t" + out}) {
                end_editor_send(command.c_str());
            }
            wait();
            const auto saveMessage = column("SELECT edit_message FROM map").front();
            send("map.open\t" + out);
            const int after = int(number("SELECT COUNT(*) FROM map_objects"));
            const auto near = [&](double x, double y) {
                return "SELECT COUNT(*) FROM map_objects WHERE template_name = '" + placed.front() + "' AND abs(x - " +
                       at(x) + ") < 0.01 AND abs(y - " + at(y) + ") < 0.01";
            };
            const bool ok = after == before + 1 && number(near(cx, cy)) == 1 && number(near(cx + 50, cy)) == 1 &&
                            number(near(cx + 100, cy)) == 0 &&
                            number("SELECT COUNT(*) FROM map_objects WHERE abs(x - " + at(x1) + ") < 0.01 AND abs(y - " +
                                   at(y1) + ") < 0.01") >= 1 &&
                            number("SELECT COUNT(*) FROM map_objects WHERE abs(angle - 1) < 0.0001") >= 1 &&
                            number("SELECT COUNT(*) FROM map_object_properties p JOIN map_objects o ON o.id = "
                                   "p.object_id WHERE p.key = 'originalOwner' AND p.value = '" + owner +
                                   "' AND o.template_name = '" + placed.front() + "'") >= 2;
            good += ok;
            if(!ok) {
                std::printf("   %s: %d objects before, %d after (%s)\n", path.c_str(), before, after, saveMessage.c_str());
            }
        }
        std::printf("-- edits: %d of %zu maps saved and read back as expected (placed %s)\n", good, paths.size(),
                    placed.empty() ? "nothing" : placed.front().c_str());
    }
    sqlite3_close(db);
    end_editor_shutdown();
    return 0;
}
