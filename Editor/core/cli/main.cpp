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
//                  undo and a redo), raises one and edits its properties, copies and pastes it,
//                  places a linked pair of waypoints and a road, saves, opens the saved file and
//                  checks what it holds (and that the preview .tga is next to it).
// --play-maps <user Maps folder> saves every listed map twice into the folder, the way the game
//                  finds user maps (<folder>/<name>/<name>.map): untouched (EdU<n>) and edited (EdE<n>:
//                  an object moved and one turned, three vehicles placed in the middle, a path of three
//                  waypoints, a road segment). Prints one line per map for scripts/qa/editor:
//                  PLAYMAP <n> <original path> <untouched file> <edited file> <vehicles placed>
#include "end_editor/editor_core_c.h"

#include "compression.h"
#include "game_files.h"
#include "storage.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace {
    using namespace end_editor;
    using namespace sqlite_orm;

    // The store, read like a client reads it: its own connection, never written (sync_schema is
    // not called; sqlite_orm 1.9.1 opens every connection read-write).
    std::optional<Storage> db;

    // Values printed the way SQLite turns them into text.
    void put(const std::string& value) {
        std::fputs(value.c_str(), stdout);
    }
    void put(bool value) {
        std::fputs(value ? "1" : "0", stdout);
    }
    void put(double value) {
        // SQLite's "%!.15g": a REAL always shows a decimal point.
        char text[40];
        std::snprintf(text, sizeof(text), "%.15g", value);
        std::string_view view(text);
        if (view.find_first_of(".ein") == std::string_view::npos) {
            std::printf("%s.0", text);
        } else if (const auto e = view.find('e');
                   e != std::string_view::npos && view.find('.') == std::string_view::npos) {
            std::printf("%.*s.0%s", int(e), text, text + e);
        } else {
            std::fputs(text, stdout);
        }
    }
    template<class T, std::enable_if_t<std::is_integral_v<T>, bool> = true>
    void put(T value) {
        std::printf("%lld", static_cast<long long>(value));
    }
    template<class T>
    void put(const std::unique_ptr<T>& value) {  // NULL prints nothing
        if (value) {
            put(*value);
        }
    }

    template<class... T>
    void printRow(const T&... values) {
        bool first = true;
        ((std::fputs(first ? "" : " | ", stdout), first = false, put(values)), ...);
        std::fputs("\n", stdout);
    }

    template<class T>
    struct IsTuple : std::false_type {};
    template<class... T>
    struct IsTuple<std::tuple<T...>> : std::true_type {};

    // Prints every row of a select, one line per row, columns separated by " | ".
    template<class Select>
    void print(Select expression) {
        for (auto&& row: db->iterate(std::move(expression))) {
            if constexpr (IsTuple<std::remove_cvref_t<decltype(row)>>::value) {
                std::apply(
                    [](const auto&... values) {
                        printRow(values...);
                    },
                    row);
            } else {
                printRow(row);
            }
        }
    }

    // The first row of a select, if there is one.
    template<class Select>
    auto first(Select expression) {
        using Row = std::remove_cvref_t<decltype(*db->iterate(expression).begin())>;
        for (auto&& row: db->iterate(std::move(expression))) {
            return std::optional<Row>(std::move(row));
        }
        return std::optional<Row>();
    }

    std::string savedPath() {
        return first(select(&MapRow::savedPath)).value_or("");
    }
    std::string editMessage() {
        return first(select(&MapRow::editMessage)).value_or("");
    }
    // The middle of the open map's playable area.
    std::pair<double, double> playableMiddle() {
        const auto area =
            first(select(columns(&MapRow::playableX0, &MapRow::playableY0, &MapRow::playableX1, &MapRow::playableY1)));
        if (!area) {
            return {0, 0};
        }
        const auto& [x0, y0, x1, y1] = *area;
        return {(x0 + x1) / 2, (y0 + y1) / 2};
    }
    // The team a placed object gets: the neutral player's, else the first player's.
    std::optional<std::string> firstTeam() {
        return first(select(&MapPlayerRow::team,
                            multi_order_by(order_by(&MapPlayerRow::neutral).desc(), order_by(&MapPlayerRow::id)),
                            limit(1)));
    }
    std::optional<std::string> firstRoad() {
        return first(
            select(&RoadTypeRow::name, where(c(&RoadTypeRow::bridge) == false), order_by(&RoadTypeRow::id), limit(1)));
    }
    // Map paths in path order, those with `only` in them (all when it is empty). Read into a list
    // before the maps are opened: a statement left open while the core writes would keep its
    // connection on the old snapshot (and hold back the WAL checkpoint).
    template<class... Conditions>
    std::vector<std::string> mapPaths(const std::string& only, Conditions... conditions) {
        std::vector<std::string> paths;
        for (auto&& path: db->iterate(select(&MapListRow::path, conditions..., order_by(&MapListRow::path)))) {
            if (path.find(only) != std::string::npos) {
                paths.push_back(std::move(path));
            }
        }
        return paths;
    }

    bool send(const std::string& command) {
        const std::string result = end_editor_send(command.c_str());
        if (!result.empty()) {
            std::fprintf(stderr, "%s: %s\n", command.c_str(), result.c_str());
            return false;
        }
        end_editor_update();
        return true;
    }
}

int main(int argc, char** argv) {
    std::string gameDir, baseDir, dbArg, modelName, objectName, animation;
    bool allModels = false, allMaps = false;
    std::string mapPath, roundTripDir, editDir, onlyMaps, playDir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() {
            return i + 1 < argc ? std::string(argv[++i]) : std::string();
        };
        if (arg == "--base") {
            baseDir = next();
        } else if (arg == "--db") {
            dbArg = next();
        } else if (arg == "--model") {
            modelName = next();
        } else if (arg == "--object") {
            objectName = next();
        } else if (arg == "--anim") {
            animation = next();
        } else if (arg == "--all-models") {
            allModels = true;
        } else if (arg == "--map") {
            mapPath = next();
        } else if (arg == "--all-maps") {
            allMaps = true;
        } else if (arg == "--roundtrip-maps") {
            roundTripDir = next();
        } else if (arg == "--edit-maps") {
            editDir = next();
        } else if (arg == "--play-maps") {
            playDir = next();
        } else if (arg == "--only") {
            onlyMaps = next();  // --edit-maps: only maps whose path has this text
        } else {
            gameDir = arg;
        }
    }
    if (gameDir.empty()) {
        std::fprintf(stderr,
                     "usage: %s <game folder> [--base <folder>] [--db <path>] [--model <name> [--anim <a>]] "
                     "[--all-models]\n",
                     argv[0]);
        return 2;
    }
    if (!end_editor_initialize(dbArg.empty() ? nullptr : dbArg.c_str())) {
        std::fprintf(stderr, "cannot open the store\n");
        return 1;
    }
    const std::string dbPath = end_editor_db_path();
    // Read the store like a client would, with its own connection.
    try {
        db.emplace(makeStorage(dbPath));
        db->open_forever();
    } catch (const std::system_error& error) {
        std::fprintf(stderr, "cannot read %s: %s\n", dbPath.c_str(), error.what());
        return 1;
    }

    if (!send("game.open\t" + gameDir + (baseDir.empty() ? "" : "\t" + baseDir))) {
        return 1;
    }
    std::printf("store: %s\n", dbPath.c_str());
    print(select(columns(&StatusRow::state, &StatusRow::message)));
    std::printf("-- objects per side\n");
    print(select(columns(&ObjectRow::side, count()), group_by(&ObjectRow::side), order_by(count()).desc(), limit(10)));
    std::printf("-- playable factions (play.faction)\n");
    print(select(columns(&FactionRow::name, &FactionRow::side), order_by(&FactionRow::id)));
    std::printf("-- warnings (first 20)\n");
    print(select(&WarningRow::text, limit(20)));

    std::string openCommand = modelName.empty() ? "" : "model.open\t" + modelName;
    if (!objectName.empty()) {
        const auto row =
            first(select(columns(&ObjectRow::model, &ObjectRow::muzzleFlashes, &ObjectRow::hiddenSubObjects),
                         where(c(&ObjectRow::name) == objectName),
                         limit(1)));
        if (!row) {
            std::fprintf(stderr, "no object %s\n", objectName.c_str());
            return 1;
        }
        const auto& [model, muzzleFlashes, hidden] = *row;
        const auto request = model + "\t" + muzzleFlashes + "\t" + hidden;
        openCommand = "model.open\t" + request;
        std::printf("-- object %s: %s\n", objectName.c_str(), request.c_str());
    }
    if (!openCommand.empty()) {
        send(openCommand);
        if (!animation.empty()) {
            send("model.animation\t" + animation);
        }
        std::printf("-- model\n");
        print(select(columns(&ModelRow::name,
                             &ModelRow::state,
                             &ModelRow::message,
                             &ModelRow::boneCount,
                             &ModelRow::animation,
                             &ModelRow::frameCount,
                             &ModelRow::frameRate,
                             length(&ModelRow::pose),
                             &ModelRow::minX,
                             &ModelRow::minY,
                             &ModelRow::minZ,
                             &ModelRow::maxX,
                             &ModelRow::maxY,
                             &ModelRow::maxZ)));
        std::printf("-- parts\n");
        for (const auto& [mesh, texture, bone, skinned, hidden, blend, houseColor, vertexBytes, indexBytes]:
             db->iterate(select(columns(&ModelPartRow::mesh,
                                        &ModelPartRow::texture,
                                        &ModelPartRow::bone,
                                        &ModelPartRow::skinned,
                                        &ModelPartRow::hidden,
                                        &ModelPartRow::blend,
                                        &ModelPartRow::houseColor,
                                        length(&ModelPartRow::vertices),
                                        length(&ModelPartRow::indices))))) {
            printRow(mesh, texture, bone, skinned, hidden, blend, houseColor, vertexBytes / 36, indexBytes / 12);
        }
        std::printf("-- textures\n");
        print(select(columns(&ModelTextureRow::name, &ModelTextureRow::width, &ModelTextureRow::height)));
        std::printf("-- animations\n");
        print(select(columns(&ModelAnimationRow::name, &ModelAnimationRow::frameCount, &ModelAnimationRow::frameRate)));
    }

    if (allModels) {
        // Read into a list before the models are opened (see mapPaths()).
        std::vector<std::string> models;
        for (auto&& model: db->iterate(
                 select(distinct(&ObjectRow::model), where(c(&ObjectRow::model) != ""), order_by(&ObjectRow::model)))) {
            models.push_back(std::move(model));
        }
        std::map<std::string, int> errors;
        std::vector<std::string> failed;
        int ready = 0, withAnimations = 0, untextured = 0;
        for (const auto& model: models) {
            send("model.open\t" + model);
            const auto [state, message] = first(select(columns(&ModelRow::state, &ModelRow::message)))
                                              .value_or(std::tuple<std::string, std::string>());
            if (state.rfind("ready", 0) == 0) {
                ++ready;
                withAnimations += db->count<ModelAnimationRow>() != 0;
                untextured += db->count<ModelTextureRow>() == 0;
            } else {
                ++errors[message.substr(0, message.find(' ', message.find(' ') + 1))];
                failed.push_back(model);
            }
        }
        std::printf("-- all models: %zu, ready %d (with animations %d, without textures %d)\n",
                    models.size(),
                    ready,
                    withAnimations,
                    untextured);
        for (const auto& [kind, count]: errors) {
            std::printf("   %d x %s...\n", count, kind.c_str());
        }
        for (std::size_t i = 0; i < failed.size() && i < 20; ++i) {
            std::printf("   failed: %s\n", failed[i].c_str());
        }
    }
    if (!mapPath.empty()) {
        if (!send("map.open\t" + mapPath)) {
            return 1;
        }
        while (end_editor_pending_work() > 0) {
            end_editor_update();
        }
        std::printf("-- map\n");
        for (const auto& [name, state, message, width, height, border, x1, y1, timeOfDay, loaded, models]:
             db->iterate(select(columns(&MapRow::name,
                                        &MapRow::state,
                                        &MapRow::message,
                                        &MapRow::width,
                                        &MapRow::height,
                                        &MapRow::border,
                                        &MapRow::playableX1,
                                        &MapRow::playableY1,
                                        &MapRow::timeOfDay,
                                        &MapRow::modelsLoaded,
                                        &MapRow::modelCount)))) {
            printRow(name,
                     state,
                     message,
                     width,
                     height,
                     border,
                     x1,
                     y1,
                     timeOfDay,
                     std::to_string(loaded) + "/" + std::to_string(models));
        }
        std::printf("-- map warnings\n");
        print(select(&MapWarningRow::text));
        std::printf("-- objects by kind\n");
        print(select(columns(&MapObjectRow::kind, count()), group_by(&MapObjectRow::kind)));
        std::printf("-- models: state, count\n");
        print(select(columns(&MapModelRow::state, count()), group_by(&MapModelRow::state)));
        for (const auto& [name, message]: db->iterate(select(columns(&MapModelRow::name, &MapModelRow::message),
                                                             where(c(&MapModelRow::state) != "ready"),
                                                             limit(20)))) {
            std::printf("failed: %s: %s\n", name.c_str(), message.c_str());
        }
        std::printf("-- terrain chunks, vertices, textures\n");
        for (const auto& [chunks, vertexBytes]:
             db->iterate(select(columns(count(), sum(length(&MapTerrainRow::vertices)))))) {
            printRow(chunks, vertexBytes ? std::to_string(std::int64_t(*vertexBytes) / 56) : std::string());
        }
        print(select(columns(&MapTextureRow::name, &MapTextureRow::width, &MapTextureRow::height)));
        for (const auto& [textures, bytes]: db->iterate(
                 select(columns(count(), sum(c(&MapModelTextureRow::width) * &MapModelTextureRow::height * 4))))) {
            printRow(textures, bytes ? std::to_string(std::int64_t(*bytes)) : std::string());
        }
        std::printf("-- water\n");
        for (const auto& [name, river, height, indexBytes]:
             db->iterate(select(columns(&MapWaterRow::name,
                                        &MapWaterRow::river,
                                        &MapWaterRow::height,
                                        length(&MapWaterRow::indices))))) {
            printRow(name, river, height, indexBytes / 12);
        }
    }

    if (allMaps) {
        const auto paths = mapPaths("");
        int ready = 0, clean = 0;
        std::map<std::string, int> warningKinds;
        std::map<std::string, long> kinds;
        long unknownTemplates = 0;
        std::vector<std::string> failed;
        for (const auto& path: paths) {
            send("map.open\t" + path);
            const auto [state, message] = first(select(columns(&MapRow::state, &MapRow::message)))
                                              .value_or(std::tuple<std::string, std::string>());
            if (state.rfind("ready", 0) == 0) {
                ++ready;
                for (const auto& [kind, objects]:
                     db->iterate(select(columns(&MapObjectRow::kind, count()), group_by(&MapObjectRow::kind)))) {
                    kinds[kind] += objects;
                }
                // from<>() is needed: count<MapObjectRow>(where(...)) would put the subquery's table
                // into the FROM clause too (sqlite_orm 1.9.1), counting a cross join.
                unknownTemplates += first(select(count(),
                                                 from<MapObjectRow>(),
                                                 where(c(&MapObjectRow::kind) == "marker" and
                                                       not_in(&MapObjectRow::templateName, select(&ObjectRow::name)))))
                                        .value_or(0);
                bool warned = false;
                for (const auto& warning: db->iterate(select(&MapWarningRow::text))) {
                    warned = true;
                    ++warningKinds[warning.substr(0, warning.find(':'))];
                    std::printf("   %s: %s\n", path.c_str(), warning.c_str());
                }
                clean += !warned;
            } else {
                failed.push_back(path + ": " + state + "|" + message);
            }
        }
        std::printf("-- all maps: %zu, ready %d, without warnings %d\n", paths.size(), ready, clean);
        for (const auto& [kind, count]: warningKinds) {
            std::printf("   %d x %s\n", count, kind.c_str());
        }
        std::printf("-- objects of all maps by kind (%ld of the markers have no known template)\n", unknownTemplates);
        for (const auto& [kind, count]: kinds) {
            std::printf("   %s: %ld\n", kind.c_str(), count);
        }
        for (const auto& item: failed) {
            std::printf("   failed: %s\n", item.c_str());
        }
    }
    if (!roundTripDir.empty()) {
        // The original bytes, read like the core reads them.
        end_editor::GameFiles files;
        std::vector<std::string> warnings;
        std::string error;
        std::vector<std::filesystem::path> folders{gameDir};
        if (!baseDir.empty()) {
            folders.emplace_back(baseDir);
        }
        files.mount(folders, warnings, error);
        std::filesystem::create_directories(roundTripDir);
        const auto paths = mapPaths("");
        const auto out = (std::filesystem::path(roundTripDir) / "roundtrip.map").string();
        int identical = 0;
        std::string packed, original, saved;
        for (const auto& path: paths) {
            if (!files.read(path, packed) || !end_editor::decompressGameData(packed, original, error)) {
                std::printf("   %s: cannot read the original\n", path.c_str());
                continue;
            }
            send("map.open\t" + path);
            send("map.save\t" + out);
            while (savedPath() != out && end_editor_pending_work() > 0 && editMessage().rfind("error", 0) != 0) {
                end_editor_update();
            }
            std::ifstream file(out, std::ios::binary);
            saved.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            if (saved == original) {
                ++identical;
                continue;
            }
            std::size_t at = 0;
            while (at < saved.size() && at < original.size() && saved[at] == original[at]) {
                ++at;
            }
            std::printf("   %s: differs at byte %zu (saved %zu bytes, original %zu): %s\n",
                        path.c_str(),
                        at,
                        saved.size(),
                        original.size(),
                        editMessage().c_str());
        }
        std::printf("-- round trip: %d of %zu maps saved byte for byte as they were\n", identical, paths.size());
    }

    if (!editDir.empty()) {
        std::filesystem::create_directories(editDir);
        const auto out = (std::filesystem::path(editDir) / "edited.map").string();
        const auto placed = first(select(&ObjectRow::name,
                                         where(c(&ObjectRow::model) != "" and like(&ObjectRow::kindOf, "%VEHICLE%")),
                                         order_by(&ObjectRow::name),
                                         limit(1)));
        const auto paths = mapPaths(onlyMaps);
        int good = 0;
        const auto wait = [&]() {
            while (end_editor_pending_work() > 0 && savedPath() != out && editMessage().rfind("error", 0) != 0) {
                end_editor_update();
            }
        };
        const auto at = [](double v) {
            return std::to_string(v);
        };
        const auto waypointLinks = [] {
            return db->count<MapLinkRow>(where(c(&MapLinkRow::kind) == "waypoint"));
        };
        const auto roadLinks = [] {
            return db->count<MapLinkRow>(where(c(&MapLinkRow::kind) == "road"));
        };
        const auto health = [] {
            return db->count<MapPropertyRow>(
                where(c(&MapPropertyRow::key) == "objectInitialHealth" and c(&MapPropertyRow::value) == "50"));
        };
        // Objects at (x, y): of the placed template, or any.
        const auto near = [&](double x, double y) {
            return db->count<MapObjectRow>(where(c(&MapObjectRow::templateName) == *placed and
                                                 abs(c(&MapObjectRow::x) - x) < 0.01 and
                                                 abs(c(&MapObjectRow::y) - y) < 0.01));
        };
        for (const auto& path: paths) {
            send("map.open\t" + path);
            const int before = db->count<MapObjectRow>();
            if (before < 3 || !placed) {
                ++good;  // nothing to edit
                continue;
            }
            const auto team = firstTeam();
            const auto road = firstRoad();
            const auto& template_ = *placed;

            // Object 1 moves by (10, 20), object 2 turns to 1 radian, the next object that is no
            // road point goes; one object is placed in the middle of the map, a second one is
            // placed, undone and redone, a third is placed and undone.
            const auto position =
                first(select(columns(&MapObjectRow::x, &MapObjectRow::y), where(c(&MapObjectRow::id) == 1)))
                    .value_or(std::tuple<double, double>());
            const double x1 = std::get<0>(position) + 10;
            const double y1 = std::get<1>(position) + 20;
            const auto [cx, cy] = playableMiddle();
            // Not a road point: one of those takes the other end of its segment along.
            const auto removedId =
                first(select(min(&MapObjectRow::id),
                             where(c(&MapObjectRow::id) >= 3 and not_in(&MapObjectRow::kind, {"road", "bridge"}))));
            const auto removed = removedId && *removedId ? std::to_string(**removedId) : std::string();
            const std::string owner = team.value_or("");
            for (const auto& command: std::vector<std::string>{
                     "map.move\t1\t" + at(x1) + "\t" + at(y1),
                     "map.rotate\t2\t1",
                     "map.delete\t" + removed,
                     "map.place\t" + template_ + "\t" + at(cx) + "\t" + at(cy) + "\t0.5\t" + owner,
                     "map.place\t" + template_ + "\t" + at(cx + 50) + "\t" + at(cy) + "\t0\t" + owner,
                     "map.undo",
                     "map.redo",
                     "map.place\t" + template_ + "\t" + at(cx + 100) + "\t" + at(cy) + "\t0\t" + owner,
                     "map.undo"}) {
                end_editor_send(command.c_str());
            }
            // Step 5, the way the client sends it: the object in the middle raised and given
            // properties, copied and pasted; two linked waypoints; a road segment.
            end_editor_update();
            // After the edits above: deleting a waypoint or a road point takes its links along.
            const int waypointsBefore = waypointLinks();
            const int roadsBefore = roadLinks();
            const int edited = db->count<MapObjectRow>();
            const int healthBefore = health();  // a removed road point takes its partner along
            const auto middle =
                first(select(&MapObjectRow::id,
                             where(abs(c(&MapObjectRow::x) - cx) < 0.01 and abs(c(&MapObjectRow::y) - cy) < 0.01),
                             limit(1)));
            std::vector<std::string> commands{"selection.set\t" + (middle ? std::to_string(*middle) : std::string("0")),
                                              "selection.height\t15",
                                              "selection.property\tobjectInitialHealth\tint\t50",
                                              "selection.property\tobjectName\tstring\tEditCheck",
                                              "edit.copy",
                                              "edit.paste\t" + at(cx) + "\t" + at(cy + 60),
                                              "placement.waypoint",
                                              "selection.set\t",
                                              "map.click_right\t" + at(cx) + "\t" + at(cy - 60) + "\t",
                                              "map.click_right\t" + at(cx + 30) + "\t" + at(cy - 60) + "\t"};
            if (road) {
                commands.insert(commands.end(),
                                {"placement.road\t" + *road,
                                 "map.click_right\t" + at(cx - 60) + "\t" + at(cy) + "\t",
                                 "map.click_right\t" + at(cx - 60) + "\t" + at(cy + 40) + "\t"});
            }
            commands.push_back("map.save\t" + out);
            for (const auto& command: commands) {
                end_editor_send(command.c_str());
            }
            wait();
            const auto saveMessage = editMessage();
            send("map.open\t" + out);
            const int after = db->count<MapObjectRow>();
            const int added = 1 + 2 + (road ? 2 : 0);  // paste, waypoints, road
            auto preview = std::filesystem::path(out);
            std::error_code sizeError;
            preview.replace_extension(".tga");
            const int raised = db->count<MapObjectRow>(
                join<MapPropertyRow>(on(c(&MapPropertyRow::objectId) == &MapObjectRow::id)),
                where(c(&MapObjectRow::height) == 15 and c(&MapPropertyRow::key) == "objectName" and
                      c(&MapPropertyRow::value) == "EditCheck"));
            const int checks[] = {raised,
                                  health() - healthBefore,
                                  near(cx, cy + 60),
                                  waypointLinks() - waypointsBefore,
                                  roadLinks() - roadsBefore};
            const bool step5 = raised == 2 && checks[1] == 2 && checks[2] == 1 && checks[3] == 1 &&
                               checks[4] == (road ? 1 : 0) &&
                               std::filesystem::file_size(preview, sizeError) == 18 + 128 * 128 * 4 + 26;
            const bool ok =
                step5 && after == edited + added && near(cx, cy) == 1 && near(cx + 50, cy) == 1 &&
                near(cx + 100, cy) == 0 &&
                db->count<MapObjectRow>(
                    where(abs(c(&MapObjectRow::x) - x1) < 0.01 and abs(c(&MapObjectRow::y) - y1) < 0.01)) >= 1 &&
                db->count<MapObjectRow>(where(abs(c(&MapObjectRow::angle) - 1) < 0.0001)) >= 1 &&
                db->count<MapPropertyRow>(join<MapObjectRow>(on(c(&MapObjectRow::id) == &MapPropertyRow::objectId)),
                                          where(c(&MapPropertyRow::key) == "originalOwner" and
                                                c(&MapPropertyRow::value) == owner and
                                                c(&MapObjectRow::templateName) == template_)) >= 2;
            good += ok;
            if (!ok) {
                std::printf("   %s: %d objects before, %d after, step 5: %d raised, %d health, %d pasted, %+d links, "
                            "%+d roads; %d after the first edits, expected %d; middle %d, cx+50 %d, cx+100 %d (%s)\n",
                            path.c_str(),
                            before,
                            after,
                            checks[0],
                            checks[1],
                            checks[2],
                            checks[3],
                            checks[4],
                            edited,
                            edited + added,
                            near(cx, cy),
                            near(cx + 50, cy),
                            near(cx + 100, cy),
                            saveMessage.c_str());
            }
        }
        std::printf("-- edits: %d of %zu maps saved and read back as expected (placed %s)\n",
                    good,
                    paths.size(),
                    placed ? placed->c_str() : "nothing");
    }
    if (!playDir.empty()) {
        const auto at = [](double v) {
            return std::to_string(v);
        };
        // A ground vehicle both games have, else the first one there is. Two selects: an ORDER BY
        // with a bound value inside multi_order_by() binds it in the LIMIT's place (sqlite_orm 1.9.1).
        const auto isGroundVehicle = c(&ObjectRow::model) != "" and like(&ObjectRow::kindOf, "%VEHICLE%") and
                                     not like(&ObjectRow::kindOf, "%AIRCRAFT%");
        auto vehicle = first(select(&ObjectRow::name,
                                    where(isGroundVehicle and c(&ObjectRow::name) == "AmericaTankCrusader"),
                                    limit(1)));
        if (!vehicle) {
            vehicle = first(select(&ObjectRow::name, where(isGroundVehicle), order_by(&ObjectRow::name), limit(1)));
        }
        const auto paths = mapPaths(onlyMaps, where(c(&MapListRow::source) != "User maps"));
        int index = 0;
        for (const auto& path: paths) {
            ++index;
            const auto saveAs = [&](const std::string& name) {
                const auto file = (std::filesystem::path(playDir) / name / (name + ".map")).string();
                end_editor_send(("map.save\t" + file).c_str());
                while (end_editor_pending_work() > 0) {
                    end_editor_update();
                }
                return savedPath() == file ? file : std::string("-");
            };
            send("map.open\t" + path);
            while (end_editor_pending_work() > 0) {
                end_editor_update();
            }
            if (first(select(&MapRow::state)).value_or("") != "ready") {
                std::printf("PLAYMAP\t%d\t%s\t-\t-\t0\n", index, path.c_str());
                continue;
            }
            const auto untouched = saveAs("EdU" + std::to_string(index));
            const auto [cx, cy] = playableMiddle();
            const auto team = firstTeam();
            const auto road = firstRoad();
            std::vector<std::string> commands;
            const auto objectOne = first(select(columns(&MapObjectRow::x, &MapObjectRow::y, &MapObjectRow::kind),
                                                where(c(&MapObjectRow::id) == 1)));
            if (objectOne && std::get<2>(*objectOne) == "model") {
                commands.push_back("map.move\t1\t" + at(std::get<0>(*objectOne) + 10) + "\t" +
                                   at(std::get<1>(*objectOne) + 10));
            }
            if (db->count<MapObjectRow>(where(c(&MapObjectRow::id) == 2)) == 1) {
                commands.push_back("map.rotate\t2\t1");
            }
            int placed = 0;
            if (vehicle) {
                commands.push_back("placement.template\t" + *vehicle);
                if (team) {
                    commands.push_back("placement.owner\t" + *team);
                }
                for (int i = 0; i < 3; ++i) {
                    commands.push_back("map.click_right\t" + at(cx + 40 * i) + "\t" + at(cy) + "\t");
                    ++placed;
                }
            }
            commands.insert(commands.end(), {"placement.waypoint", "selection.set\t"});
            for (int i = 0; i < 3; ++i) {
                commands.push_back("map.click_right\t" + at(cx + 40 * i) + "\t" + at(cy - 80) + "\t");
            }
            if (road) {
                commands.insert(commands.end(),
                                {"placement.road\t" + *road,
                                 "map.click_right\t" + at(cx - 80) + "\t" + at(cy) + "\t",
                                 "map.click_right\t" + at(cx - 80) + "\t" + at(cy + 80) + "\t"});
            }
            commands.push_back("placement.object");
            for (const auto& command: commands) {
                const std::string result = end_editor_send(command.c_str());
                if (!result.empty()) {
                    std::fprintf(stderr, "%s: %s: %s\n", path.c_str(), command.c_str(), result.c_str());
                }
                while (end_editor_pending_work() > 0) {
                    end_editor_update();
                }
            }
            const auto edited = saveAs("EdE" + std::to_string(index));
            std::printf("PLAYMAP\t%d\t%s\t%s\t%s\t%d\n",
                        index,
                        path.c_str(),
                        untouched.c_str(),
                        edited.c_str(),
                        placed);
            std::fflush(stdout);
        }
    }
    db.reset();
    end_editor_shutdown();
    return 0;
}
