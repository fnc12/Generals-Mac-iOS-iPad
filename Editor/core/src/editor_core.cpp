#include "editor_core.h"

#include "game_data.h"
#include "map_scene.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>
#include <system_error>

namespace end_editor {

    namespace {
        std::vector<std::string> splitTabs(std::string_view text) {
            std::vector<std::string> parts;
            std::size_t start = 0;
            while(true) {
                const auto tab = text.find('\t', start);
                parts.emplace_back(text.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
                if(tab == std::string_view::npos) {
                    return parts;
                }
                start = tab + 1;
            }
        }

        // A number of a command argument ("12.5"), independent of the C locale.
        bool parseNumber(const std::string &text, double &value) {
            std::istringstream stream(text);
            stream.imbue(std::locale::classic());
            stream >> value;
            return !text.empty() && stream && stream.peek() == std::char_traits<char>::eof() && std::isfinite(value);
        }

        bool isInside(const std::filesystem::path &path, const std::filesystem::path &folder) {
            std::error_code error;
            const auto full = std::filesystem::weakly_canonical(path, error);
            const auto base = std::filesystem::weakly_canonical(folder, error);
            const auto relative = full.lexically_relative(base);
            return !base.empty() && !relative.empty() && *relative.begin() != "..";
        }

        template<class T>
        std::vector<char> toBlob(const std::vector<T> &items) {
            std::vector<char> blob(items.size() * sizeof(T));
            if(!blob.empty()) {
                std::memcpy(blob.data(), items.data(), blob.size());
            }
            return blob;
        }
    }

    EditorCore::EditorCore(std::string dbPath) : path(std::move(dbPath)) {
        this->storage = std::make_unique<Storage>(makeStorage(this->path));
        this->storage->open_forever();
        this->storage->pragma.journal_mode(sqlite_orm::journal_mode::WAL);
        this->storage->sync_schema();
        if(auto previous = this->storage->get_pointer<StatusRow>(1)) {
            this->revision = previous->revision;
        }
        if(auto previous = this->storage->get_pointer<ModelRow>(1)) {
            this->revision = std::max(this->revision, previous->revision);
        }
        if(auto previous = this->storage->get_pointer<MapRow>(1)) {
            this->revision = std::max({this->revision, previous->revision, previous->modelRevision});
        }
        this->setStatus("idle", "", "", 0);
        this->setModelState("", "idle", "");
        this->storage->transaction([&] {
            this->storage->remove_all<MapListRow>();
            this->clearMap();
            this->setMapState("", "idle", "");
            return true;
        });
    }

    std::string EditorCore::send(std::string_view command) {
        const auto tab = command.find('\t');
        const auto name = command.substr(0, tab);
        const auto arguments =
            tab == std::string_view::npos ? std::vector<std::string>() : splitTabs(command.substr(tab + 1));
        const bool hasArgument = !arguments.empty() && !arguments.front().empty();

        if(name == "game.open") {
            if(!hasArgument) {
                return "error:missing_argument";
            }
            // Status changes now so the client shows "loading" before the work in update() starts.
            this->setStatus("loading", "Reading game data", arguments.front(), 0);
            this->jobs.push_back(Job{Job::openGame, arguments});
            return "";
        }
        if(name == "model.open") {
            if(!hasArgument) {
                return "error:missing_argument";
            }
            if(!this->loader) {
                return "error:no_game";
            }
            this->setModelState(arguments.front(), "loading", "");
            this->jobs.push_back(Job{Job::openModel, arguments});
            return "";
        }
        if(name == "model.animation") {
            if(!this->loader) {
                return "error:no_game";
            }
            this->jobs.push_back(Job{Job::playAnimation, arguments.empty() ? std::vector<std::string>{""} : arguments});
            return "";
        }
        if(name == "map.open") {
            if(!hasArgument) {
                return "error:missing_argument";
            }
            if(!this->loader) {
                return "error:no_game";
            }
            this->setMapState(arguments.front(), "loading", "Reading the map");
            this->jobs.push_back(Job{Job::openMap, arguments});
            return "";
        }
        if(name == "maps.user") {
            this->userMapsFolder = hasArgument ? arguments.front() : "";
            if(this->loader) {
                this->jobs.push_back(Job{Job::listMaps, {}});
            }
            return "";
        }
        const bool mapQueued = std::any_of(this->jobs.begin(), this->jobs.end(),
                                           [](const Job &job) { return job.kind == Job::openMap; });
        if(name == "map.place" || name == "map.move" || name == "map.rotate" || name == "map.delete" ||
           name == "map.undo" || name == "map.redo") {
            if(!this->document && !mapQueued) {
                return "error:no_map";
            }
            // Arguments: place <template> <x> <y> <angle> <team>, move <id> <x> <y>,
            // rotate <id> <angle>, delete <id>.
            const std::size_t needed = name == "map.place" ? 5 : name == "map.move" ? 3 : name == "map.rotate" ? 2
                                                                : name == "map.delete"  ? 1
                                                                                        : 0;
            if(arguments.size() < needed || (needed > 0 && !hasArgument)) {
                return "error:missing_argument";
            }
            const std::size_t firstNumber = name == "map.place" ? 1 : 0;
            const std::size_t numbers = name == "map.place" ? 3 : needed;
            for(std::size_t i = firstNumber; i < firstNumber + numbers; ++i) {
                double value = 0;
                if(!parseNumber(arguments[i], value)) {
                    return "error:bad_argument";
                }
            }
            if(name == "map.place" && !this->findObject(arguments.front())) {
                return "error:unknown_template";
            }
            std::vector<std::string> edit{std::string(name)};
            edit.insert(edit.end(), arguments.begin(), arguments.begin() + std::ptrdiff_t(std::min(needed, arguments.size())));
            this->queueBeforeModelLoading(Job{Job::editMap, edit});
            return "";
        }
        if(name == "map.save") {
            if(!hasArgument) {
                return "error:missing_argument";
            }
            if(!this->document && !mapQueued) {
                return "error:no_map";
            }
            this->queueBeforeModelLoading(Job{Job::saveMap, arguments});
            return "";
        }
        return "error:unknown_command";
    }

    void EditorCore::queueBeforeModelLoading(Job job) {
        // Model loading re-queues itself at the back; an edit or a save goes before it so it runs
        // in the next update and the client sees it at once.
        auto at = this->jobs.end();
        while(at != this->jobs.begin() && std::prev(at)->kind == Job::loadMapModels) {
            --at;
        }
        this->jobs.insert(at, std::move(job));
    }

    void EditorCore::update() {
        // A later request can make a queued one pointless (the user clicked on); those are
        // dropped without using up the update.
        const auto supersededBy = [&](auto predicate) {
            for(const auto &later: this->jobs) {
                if(predicate(later.kind)) {
                    return true;
                }
            }
            return false;
        };
        while(!this->jobs.empty()) {
            const auto job = std::move(this->jobs.front());
            this->jobs.pop_front();
            switch(job.kind) {
                case Job::openGame:
                    this->runOpenGame(job);
                    return;
                case Job::openModel:
                    if(supersededBy([](auto kind) { return kind == Job::openModel || kind == Job::openGame; })) {
                        continue;
                    }
                    this->runOpenModel(job.arguments);
                    return;
                case Job::playAnimation:
                    if(supersededBy([](auto kind) {
                           return kind == Job::playAnimation || kind == Job::openModel || kind == Job::openGame;
                       })) {
                        continue;
                    }
                    this->runPlayAnimation(job.arguments.front());
                    return;
                case Job::openMap:
                    if(supersededBy([](auto kind) { return kind == Job::openMap || kind == Job::openGame; })) {
                        continue;
                    }
                    this->runOpenMap(job.arguments.front());
                    return;
                case Job::loadMapModels:
                    if(supersededBy([](auto kind) { return kind == Job::openMap || kind == Job::openGame; })) {
                        continue;
                    }
                    this->runLoadMapModels();
                    return;
                case Job::editMap:
                    // Edits are quick: every one queued runs now, in order, in one transaction.
                    this->storage->transaction([&] {
                        this->runEditMap(job.arguments);
                        while(!this->jobs.empty() && this->jobs.front().kind == Job::editMap) {
                            const auto next = std::move(this->jobs.front());
                            this->jobs.pop_front();
                            this->runEditMap(next.arguments);
                        }
                        return true;
                    });
                    return;
                case Job::saveMap:
                    this->runSaveMap(job.arguments.front());
                    return;
                case Job::listMaps:
                    this->relistMaps();
                    return;
            }
        }
    }

    void EditorCore::setStatus(const std::string &state, const std::string &message, const std::string &gameDir,
                               std::int64_t objectCount) {
        StatusRow row;
        row.state = state;
        row.message = message;
        row.gameDir = gameDir;
        row.objectCount = objectCount;
        row.revision = ++this->revision;
        row.mapListRevision = this->mapListRevision;
        this->storage->replace(row);
    }

    void EditorCore::setModelState(const std::string &name, const std::string &state, const std::string &message) {
        ModelRow row;
        row.name = name;
        row.state = state;
        row.message = message;
        row.revision = ++this->revision;
        this->storage->replace(row);
    }

    void EditorCore::runOpenGame(const Job &job) {
        std::vector<std::filesystem::path> folders(job.arguments.begin(), job.arguments.end());
        this->gameFolders = folders;
        this->loader.reset();
        this->model = LoadedModel{};
        const auto scan = scanGameData(folders, this->files);
        const auto &gameDir = job.arguments.front();
        this->storage->transaction([&] {
            this->storage->remove_all<ObjectRow>();
            this->storage->remove_all<WarningRow>();
            this->storage->remove_all<ModelPartRow>();
            this->storage->remove_all<ModelBoneRow>();
            this->storage->remove_all<ModelTextureRow>();
            this->storage->remove_all<ModelAnimationRow>();
            this->setModelState("", "idle", "");
            for(const auto &warning: scan.warnings) {
                this->storage->insert(WarningRow{0, warning});
            }
            this->storage->remove_all<MapListRow>();
            this->clearMap();
            this->setMapState("", "idle", "");
            if(!scan.error.empty()) {
                this->setStatus("error", scan.error, gameDir, 0);
                return true;
            }
            for(const auto &object: scan.objects) {
                ObjectRow row;
                row.name = object.name;
                row.definition = object.definition;
                row.parent = object.parent;
                row.side = object.side;
                row.kindOf = object.kindOf;
                row.buildable = object.buildable;
                row.editorSorting = object.editorSorting;
                row.displayName = object.displayName;
                row.model = object.model;
                row.muzzleFlashes = object.muzzleFlashes;
                row.hiddenSubObjects = object.hiddenSubObjects;
                row.sourceFile = object.sourceFile;
                row.line = object.line;
                this->storage->insert(row);
            }
            this->listMaps();
            this->setStatus("ready",
                            std::to_string(scan.objects.size()) + " objects from " +
                                std::to_string(scan.iniFileCount) + " INI files in " +
                                std::to_string(scan.archiveCount) + " archives, " +
                                std::to_string(scan.warnings.size()) + " warnings",
                            gameDir,
                            std::int64_t(scan.objects.size()));
            return true;
        });
        this->objectInfos.clear();
        this->objectsByName.clear();
        if(scan.error.empty()) {
            this->loader = std::make_unique<ModelLoader>(this->files);
            this->objectInfos = scan.objects;
            for(std::size_t i = 0; i < this->objectInfos.size(); ++i) {
                auto key = this->objectInfos[i].name;
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                this->objectsByName.emplace(key, i);
            }
        }
    }

    void EditorCore::runOpenModel(const std::vector<std::string> &arguments) {
        const std::string &name = arguments.front();
        ModelLoader::Hiding hiding;
        if(arguments.size() > 1) {
            hiding.muzzleFlashes = arguments[1];
        }
        if(arguments.size() > 2) {
            hiding.subObjects = arguments[2];
        }
        this->model = this->loader->load(name, hiding);
        std::string poseError;
        const auto pose = this->model.error.empty() ? this->loader->pose(this->model, "", poseError) : ModelPose{};
        this->storage->transaction([&] {
            this->storage->remove_all<ModelPartRow>();
            this->storage->remove_all<ModelBoneRow>();
            this->storage->remove_all<ModelTextureRow>();
            this->storage->remove_all<ModelAnimationRow>();
            if(!this->model.error.empty()) {
                this->setModelState(name, "error", this->model.error);
                return true;
            }
            for(const auto &part: this->model.parts) {
                ModelPartRow row;
                row.mesh = part.mesh;
                row.texture = part.texture;
                row.bone = int(part.bone);
                row.skinned = part.skinned;
                row.twoSided = part.twoSided;
                row.hidden = part.hidden;
                row.blend = int(part.blend);
                row.houseColor = part.houseColor;
                row.vertices = toBlob(part.vertices);
                row.indices = toBlob(part.indices);
                this->storage->insert(row);
            }
            const auto &pivots = this->model.hierarchy.pivots;
            for(std::size_t i = 0; i < pivots.size(); ++i) {
                this->storage->insert(ModelBoneRow{0, int(i), pivots[i].name, pivots[i].parent});
            }
            for(const auto &[textureName, image]: this->model.textures) {
                this->storage->insert(
                    ModelTextureRow{0, textureName, int(image.width), int(image.height), toBlob(image.rgba)});
            }
            for(const auto &animation: this->model.animations) {
                this->storage->insert(
                    ModelAnimationRow{0, animation.name, int(animation.frameCount), int(animation.frameRate)});
            }
            ModelRow row;
            row.name = name;
            row.state = "ready";
            row.message = std::to_string(this->model.parts.size()) + " parts, " + std::to_string(pivots.size()) +
                          " bones, " + std::to_string(this->model.animations.size()) + " animations, " +
                          std::to_string(this->model.warnings.size()) + " warnings";
            row.revision = ++this->revision;
            row.boneCount = int(pivots.size());
            row.frameCount = int(pose.frameCount);
            row.frameRate = int(pose.frameRate);
            row.pose = toBlob(pose.matrices);
            row.visible = toBlob(pose.visible);
            row.minX = this->model.boundsMin[0];
            row.minY = this->model.boundsMin[1];
            row.minZ = this->model.boundsMin[2];
            row.maxX = this->model.boundsMax[0];
            row.maxY = this->model.boundsMax[1];
            row.maxZ = this->model.boundsMax[2];
            this->storage->replace(row);
            return true;
        });
    }

    void EditorCore::runPlayAnimation(const std::string &name) {
        auto row = this->storage->get_pointer<ModelRow>(1);
        if(!row || row->state != "ready" || !this->model.error.empty() || this->model.parts.empty()) {
            return;
        }
        std::string error;
        const auto pose = this->loader->pose(this->model, name, error);
        row->animation = error.empty() ? name : "";
        row->message = error.empty() ? row->message : error;
        row->frameCount = int(pose.frameCount);
        row->frameRate = int(pose.frameRate);
        row->pose = toBlob(pose.matrices);
        row->visible = toBlob(pose.visible);
        row->revision = ++this->revision;
        this->storage->replace(*row);
    }

    void EditorCore::setMapState(const std::string &path, const std::string &state, const std::string &message) {
        MapRow row;
        row.path = path;
        row.state = state;
        row.message = message;
        row.revision = ++this->revision;
        this->storage->replace(row);
    }

    void EditorCore::clearMap() {
        this->storage->remove_all<MapTextureRow>();
        this->storage->remove_all<MapTerrainRow>();
        this->storage->remove_all<MapWaterRow>();
        this->storage->remove_all<MapObjectRow>();
        this->storage->remove_all<MapPropertyRow>();
        this->storage->remove_all<MapModelRow>();
        this->storage->remove_all<MapModelPartRow>();
        this->storage->remove_all<MapModelTextureRow>();
        this->storage->remove_all<MapWarningRow>();
        this->storage->remove_all<MapPlayerRow>();
        this->mapModels.clear();
        this->nextMapModel = 0;
        this->mapTextures.clear();
        this->mapModelIds.clear();
        this->mapModelJobQueued = false;
        this->document.reset();
    }

    void EditorCore::listMaps() {
        const auto paths = this->files.list([](const std::string &path) {
            return path.rfind("maps/", 0) == 0 && path.size() > 4 && path.compare(path.size() - 4, 4, ".map") == 0;
        });
        for(const auto &path: paths) {
            MapListRow row;
            row.path = path;
            const auto original = this->files.originalPath(path);
            // "Maps/Alpine Assault/Alpine Assault.map" -> "Alpine Assault"
            const auto slash = original.rfind('/');
            const auto folderStart = original.rfind('/', slash == std::string::npos || slash == 0 ? 0 : slash - 1);
            row.name = slash == std::string::npos || folderStart == std::string::npos || folderStart >= slash
                           ? original
                           : original.substr(folderStart + 1, slash - folderStart - 1);
            row.source = this->files.sourceOf(path);
            this->storage->insert(row);
        }
        // The game's user maps: Maps/<name>/<name>.map (MapCache::loadMapsFromDisk), opened by
        // their path on disk.
        std::error_code error;
        if(this->userMapsFolder.empty() || !std::filesystem::is_directory(this->userMapsFolder, error)) {
            return;
        }
        std::vector<std::filesystem::path> userMaps;
        for(const auto &folder: std::filesystem::directory_iterator(this->userMapsFolder, error)) {
            if(!folder.is_directory(error)) {
                continue;
            }
            for(const auto &file: std::filesystem::directory_iterator(folder.path(), error)) {
                auto extension = file.path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
                if(file.is_regular_file(error) && extension == ".map") {
                    userMaps.push_back(file.path());
                }
            }
        }
        std::sort(userMaps.begin(), userMaps.end());
        for(const auto &file: userMaps) {
            MapListRow row;
            row.path = file.string();
            row.name = file.parent_path().filename().string();
            row.source = "User maps";
            row.size = std::int64_t(std::filesystem::file_size(file, error));
            this->storage->insert(row);
        }
    }

    void EditorCore::relistMaps() {
        this->storage->transaction([&] {
            this->storage->remove_all<MapListRow>();
            this->listMaps();
            // Only the list changed: the status revision stays (a client re-reads everything on it).
            if(auto status = this->storage->get_pointer<StatusRow>(1)) {
                status->mapListRevision = ++this->mapListRevision;
                this->storage->replace(*status);
            }
            return true;
        });
    }

    const ObjectInfo *EditorCore::findObject(const std::string &templateName) const {
        auto key = templateName;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const auto found = this->objectsByName.find(key);
        return found == this->objectsByName.end() ? nullptr : &this->objectInfos[found->second];
    }

    void EditorCore::runOpenMap(const std::string &path) {
        // A game path ("maps/x/x.map") or a .map file anywhere on disk.
        std::string bytes;
        std::string name;
        const auto normalized = normalizeArchivePath(path);
        if(this->files.exists(normalized)) {
            this->files.read(normalized, bytes);
            name = std::filesystem::path(this->files.originalPath(normalized)).stem().string();
        } else {
            std::ifstream file(std::filesystem::path(path), std::ios::binary);
            if(!file) {
                this->storage->transaction([&] {
                    this->clearMap();
                    this->setMapState(path, "error", "no such map");
                    return true;
                });
                return;
            }
            bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            name = std::filesystem::path(path).stem().string();
        }

        MapFile map;
        std::string error;
        const bool parsed = parseMap(bytes, map, error);
        const auto scene = parsed ? buildMapScene(map, this->files, [&](const std::string &templateName) {
            return this->findObject(templateName);
        })
                                  : MapScene{};
        this->storage->transaction([&] {
            this->clearMap();
            if(!parsed) {
                this->setMapState(path, "error", error);
                return true;
            }
            for(const auto &warning: map.warnings) {
                this->storage->insert(MapWarningRow{0, warning});
            }
            for(const auto &warning: scene.warnings) {
                this->storage->insert(MapWarningRow{0, warning});
            }
            this->storage->insert(MapTextureRow{0, "terrain", int(scene.atlas.width), int(scene.atlas.height),
                                                toBlob(scene.atlas.rgba)});
            if(scene.waterTexture.width > 0) {
                this->storage->insert(MapTextureRow{0, "water", int(scene.waterTexture.width),
                                                    int(scene.waterTexture.height), toBlob(scene.waterTexture.rgba)});
            }
            for(const auto &chunk: scene.chunks) {
                this->storage->insert(MapTerrainRow{0, chunk.x, chunk.y, toBlob(chunk.vertices), toBlob(chunk.indices)});
            }
            for(const auto &water: scene.water) {
                this->storage->insert(MapWaterRow{0, water.name, water.river, water.height, toBlob(water.vertices),
                                                  toBlob(water.indices)});
            }

            for(const auto &player: mapPlayers(map)) {
                this->storage->insert(MapPlayerRow{0, player.name, player.displayName, player.team,
                                                   int(player.color), player.neutral});
            }
            const auto objectCount = scene.objects.size();
            this->document = std::make_unique<MapDocument>(std::move(map));
            // The document numbers the objects the engine keeps 1.. in file order, like the scene.
            for(const auto &entry: this->document->objects()) {
                if(entry.id != 0) {
                    this->writeMapObject(entry.id);
                }
            }
            const auto &opened = this->document->map();

            MapRow row;
            row.path = path;
            row.name = name;
            row.state = "ready";
            row.message = std::to_string(opened.width) + "×" + std::to_string(opened.height) + ", " +
                          std::to_string(objectCount) + " objects, " + std::to_string(this->mapModelIds.size()) +
                          " models, " + std::to_string(scene.water.size()) + " water areas, " +
                          std::to_string(opened.warnings.size() + scene.warnings.size()) + " warnings";
            row.revision = ++this->revision;
            row.width = opened.width;
            row.height = opened.height;
            row.border = opened.border;
            row.playableX0 = scene.playable[0];
            row.playableY0 = scene.playable[1];
            row.playableX1 = scene.playable[2];
            row.playableY1 = scene.playable[3];
            row.timeOfDay = opened.lighting.timeOfDay;
            const auto time = std::size_t(std::clamp(opened.lighting.timeOfDay, 1, 4) - 1);
            std::vector<float> lighting;
            for(const auto *light: {&opened.lighting.terrain[time], &opened.lighting.objects[time]}) {
                lighting.insert(lighting.end(), light->ambient.begin(), light->ambient.end());
                lighting.insert(lighting.end(), light->diffuse.begin(), light->diffuse.end());
                lighting.insert(lighting.end(), light->direction.begin(), light->direction.end());
            }
            if(!opened.hasLighting) {  // no GlobalLighting chunk: a plain afternoon sun
                lighting = {0.4f, 0.4f, 0.4f, 0.8f, 0.8f, 0.8f, -0.8f, 0.4f, -0.45f,
                            0.4f, 0.4f, 0.4f, 0.8f, 0.8f, 0.8f, -0.8f, 0.4f, -0.45f};
            }
            row.lighting = toBlob(lighting);
            row.heights = toBlob(opened.heights);
            row.objectCount = int(objectCount);
            row.modelCount = int(this->mapModelIds.size());
            row.modelRevision = row.revision;
            row.objectRevision = row.revision;
            const auto &look = scene.waterLook;
            row.waterR = look.standingColor[0] * look.diffuse[0];
            row.waterG = look.standingColor[1] * look.diffuse[1];
            row.waterB = look.standingColor[2] * look.diffuse[2];
            row.waterA = look.transparentDiffuse[3];
            row.waterRepeat = look.repeatCount;
            this->storage->replace(row);
            return true;
        });
        // writeMapObject() queued the model loading when the map uses any model.
    }

    void EditorCore::runLoadMapModels() {
        // Loads models for a short while per update so the client stays responsive, then queues
        // itself again.
        const auto start = std::chrono::steady_clock::now();
        const auto budget = std::chrono::milliseconds(40);
        int loaded = 0;
        this->storage->transaction([&] {
            while(this->nextMapModel < this->mapModels.size() &&
                  (loaded == 0 || std::chrono::steady_clock::now() - start < budget)) {
                const auto &pending = this->mapModels[this->nextMapModel++];
                ++loaded;
                const auto request = splitTabs(pending.request);
                ModelLoader::Hiding hiding;
                hiding.muzzleFlashes = request.size() > 1 ? request[1] : "";
                hiding.subObjects = request.size() > 2 ? request[2] : "";
                auto model = this->loader->load(request.front(), hiding, false);
                MapModelRow row;
                row.id = pending.id;
                row.name = request.front();
                if(!model.error.empty()) {
                    row.state = "error";
                    row.message = model.error;
                    this->storage->replace(row);
                    continue;
                }
                std::string poseError;
                const auto pose = this->loader->pose(model, "", poseError);
                const auto boneCount = model.hierarchy.pivots.size();
                std::array<float, 3> low{1e30f, 1e30f, 1e30f}, high{-1e30f, -1e30f, -1e30f};
                int parts = 0;
                for(auto &part: model.parts) {
                    const bool boneHidden = !part.skinned && part.bone < pose.visible.size() && pose.visible[part.bone] == 0;
                    if(part.hidden || boneHidden || part.vertices.empty()) {
                        continue;
                    }
                    // Into the bind pose: the map draws every model standing still.
                    for(auto &vertex: part.vertices) {
                        const auto bone = vertex.bone < boneCount && vertex.bone < pose.matrices.size() ? vertex.bone : 0;
                        if(bone < pose.matrices.size()) {
                            const auto &m = pose.matrices[bone];
                            const float p[3] = {vertex.position[0], vertex.position[1], vertex.position[2]};
                            const float n[3] = {vertex.normal[0], vertex.normal[1], vertex.normal[2]};
                            for(int i = 0; i < 3; ++i) {
                                vertex.position[i] = m[std::size_t(i)] * p[0] + m[std::size_t(4 + i)] * p[1] +
                                                     m[std::size_t(8 + i)] * p[2] + m[std::size_t(12 + i)];
                                vertex.normal[i] =
                                    m[std::size_t(i)] * n[0] + m[std::size_t(4 + i)] * n[1] + m[std::size_t(8 + i)] * n[2];
                            }
                        }
                        vertex.bone = 0;
                        for(int i = 0; i < 3; ++i) {
                            low[std::size_t(i)] = std::min(low[std::size_t(i)], vertex.position[i]);
                            high[std::size_t(i)] = std::max(high[std::size_t(i)], vertex.position[i]);
                        }
                    }
                    MapModelPartRow partRow;
                    partRow.modelId = pending.id;
                    partRow.texture = part.texture;
                    partRow.blend = int(part.blend);
                    partRow.houseColor = part.houseColor;
                    partRow.twoSided = part.twoSided;
                    partRow.vertices = toBlob(part.vertices);
                    partRow.indices = toBlob(part.indices);
                    this->storage->insert(partRow);
                    ++parts;
                    if(!part.texture.empty() && !this->mapTextures.count(part.texture)) {
                        const auto found = model.textures.find(part.texture);
                        if(found != model.textures.end()) {
                            this->mapTextures.insert(part.texture);
                            auto image = shrinkImage(found->second, 512);
                            this->storage->insert(MapModelTextureRow{0, part.texture, int(image.width),
                                                                     int(image.height), toBlob(image.rgba)});
                        }
                    }
                }
                row.state = parts > 0 ? "ready" : "error";
                row.message = parts > 0 ? std::to_string(parts) + " parts" : "nothing visible";
                if(parts > 0) {
                    row.minX = low[0];
                    row.minY = low[1];
                    row.minZ = low[2];
                    row.maxX = high[0];
                    row.maxY = high[1];
                    row.maxZ = high[2];
                }
                this->storage->replace(row);
            }
            if(auto map = this->storage->get_pointer<MapRow>(1)) {
                map->modelsLoaded = int(this->nextMapModel);
                map->modelRevision = ++this->revision;
                this->storage->replace(*map);
            }
            return true;
        });
        this->mapModelJobQueued = this->nextMapModel < this->mapModels.size();
        if(this->mapModelJobQueued) {
            this->jobs.push_back(Job{Job::loadMapModels, {}});
        }
    }

    void EditorCore::writeMapObject(std::int64_t id) {
        using namespace sqlite_orm;
        this->storage->remove_all<MapPropertyRow>(where(c(&MapPropertyRow::objectId) == id));
        const auto *entry = this->document ? this->document->find(id) : nullptr;
        if(!entry) {
            this->storage->remove<MapObjectRow>(id);
            return;
        }
        const auto placed = resolveObject(this->document->map(), entry->object,
                                          [&](const std::string &name) { return this->findObject(name); });
        MapObjectRow row;
        row.id = id;
        row.templateName = placed.templateName;
        row.kind = placed.kind;
        row.x = placed.x;
        row.y = placed.y;
        row.z = placed.z;
        row.angle = placed.angle;
        row.flags = placed.flags;
        row.owner = placed.owner;
        row.color = int(placed.color);
        if(!placed.modelKey.empty()) {
            // One map_models row per distinct model request, in first-use order.
            auto [it, added] = this->mapModelIds.emplace(placed.modelKey, std::int64_t(this->mapModelIds.size() + 1));
            if(added) {
                MapModelRow model;
                model.id = it->second;
                model.name = placed.model;
                model.state = "pending";
                this->storage->replace(model);
                this->mapModels.push_back(PendingMapModel{it->second, placed.modelKey});
                if(!this->mapModelJobQueued && this->loader) {
                    this->jobs.push_back(Job{Job::loadMapModels, {}});
                    this->mapModelJobQueued = true;
                }
            }
            row.modelId = it->second;
        }
        this->storage->replace(row);
        for(const auto &value: placed.properties) {
            this->storage->insert(MapPropertyRow{0, id, value.key, dictTypeName(value.type), value.toString()});
        }
    }

    void EditorCore::updateMapEditState(const std::string &message) {
        auto row = this->storage->get_pointer<MapRow>(1);
        if(!row || !this->document) {
            return;
        }
        row->objectRevision = ++this->revision;
        row->undoCount = int(this->document->undoCount());
        row->redoCount = int(this->document->redoCount());
        row->edited = this->document->edited();
        row->lastObject = this->document->lastChanged();
        row->editMessage = message;
        row->modelCount = int(this->mapModelIds.size());
        row->objectCount = int(std::count_if(this->document->objects().begin(), this->document->objects().end(),
                                             [](const auto &entry) { return entry.id != 0 && !entry.removed; }));
        this->storage->replace(*row);
    }

    void EditorCore::runEditMap(const std::vector<std::string> &arguments) {
        if(!this->document || arguments.empty()) {
            return;
        }
        const auto &name = arguments.front();
        const auto number = [&](std::size_t i) {
            double value = 0;
            parseNumber(i < arguments.size() ? arguments[i] : "", value);
            return value;
        };
        bool done = false;
        if(name == "map.place") {
            const auto &team = arguments.size() > 5 ? arguments[5] : std::string();
            this->document->place(arguments[1], float(number(2)), float(number(3)), float(number(4)), team);
            done = true;
        } else if(name == "map.move") {
            done = this->document->move(std::int64_t(number(1)), float(number(2)), float(number(3)));
        } else if(name == "map.rotate") {
            done = this->document->rotate(std::int64_t(number(1)), float(number(2)));
        } else if(name == "map.delete") {
            done = this->document->remove(std::int64_t(number(1)));
        } else if(name == "map.undo") {
            done = this->document->undo();
        } else if(name == "map.redo") {
            done = this->document->redo();
        }
        if(done) {
            this->writeMapObject(this->document->lastChanged());
        }
        this->updateMapEditState(done ? "" : name == "map.undo" ? "Nothing to undo"
                                             : name == "map.redo" ? "Nothing to redo"
                                                                  : "No such object");
    }

    void EditorCore::runSaveMap(const std::string &path) {
        if(!this->document) {
            return;
        }
        const std::filesystem::path target(path);
        std::string message;
        for(const auto &folder: this->gameFolders) {
            if(isInside(target, folder)) {
                message = "error: not saved, " + path + " is inside the game folder";
            }
        }
        if(message.empty()) {
            const auto bytes = this->document->save();
            std::error_code error;
            if(target.has_parent_path()) {
                std::filesystem::create_directories(target.parent_path(), error);
            }
            // Written next to the target, then renamed over it: a failed write leaves no half file.
            auto temporary = target;
            temporary += ".saving";
            {
                std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
                file.write(bytes.data(), std::streamsize(bytes.size()));
                if(!file) {
                    message = "error: cannot write " + temporary.string();
                }
            }
            if(message.empty()) {
                std::filesystem::rename(temporary, target, error);
                if(error) {
                    message = "error: cannot write " + path + ": " + error.message();
                }
            }
            if(!message.empty()) {
                std::filesystem::remove(temporary, error);
            }
        }
        this->storage->transaction([&] {
            if(message.empty()) {
                this->document->markSaved();
                if(auto row = this->storage->get_pointer<MapRow>(1)) {
                    row->savedPath = path;
                    this->storage->replace(*row);
                }
                message = "Saved " + path;
            }
            this->updateMapEditState(message);
            return true;
        });
        if(!this->userMapsFolder.empty() && isInside(target, this->userMapsFolder)) {
            this->relistMaps();
        }
    }
}
