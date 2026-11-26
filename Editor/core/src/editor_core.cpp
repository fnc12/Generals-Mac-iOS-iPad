#include "editor_core.h"

#include "game_data.h"
#include "map_scene.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

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
        return "error:unknown_command";
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
                    if(supersededBy([](auto kind) { return kind != Job::openMap && kind != Job::loadMapModels; })) {
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
        this->mapModels.clear();
        this->nextMapModel = 0;
        this->mapTextures.clear();
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

            // One map_models row per distinct model request, in first-use order.
            std::map<std::string, std::int64_t> modelIds;
            std::int64_t objectId = 0;
            for(const auto &object: scene.objects) {
                MapObjectRow row;
                row.id = ++objectId;
                row.templateName = object.templateName;
                row.kind = object.kind;
                row.x = object.x;
                row.y = object.y;
                row.z = object.z;
                row.angle = object.angle;
                row.flags = object.flags;
                row.owner = object.owner;
                row.color = int(object.color);
                if(!object.modelKey.empty()) {
                    auto [it, added] = modelIds.emplace(object.modelKey, std::int64_t(modelIds.size() + 1));
                    if(added) {
                        MapModelRow model;
                        model.id = it->second;
                        model.name = object.model;
                        model.state = "pending";
                        this->storage->replace(model);
                        this->mapModels.push_back(PendingMapModel{it->second, object.modelKey});
                    }
                    row.modelId = it->second;
                }
                this->storage->replace(row);
                for(const auto &value: object.properties) {
                    this->storage->insert(
                        MapPropertyRow{0, row.id, value.key, dictTypeName(value.type), value.toString()});
                }
            }

            MapRow row;
            row.path = path;
            row.name = name;
            row.state = "ready";
            row.message = std::to_string(map.width) + "×" + std::to_string(map.height) + ", " +
                          std::to_string(scene.objects.size()) + " objects, " + std::to_string(modelIds.size()) +
                          " models, " + std::to_string(scene.water.size()) + " water areas, " +
                          std::to_string(map.warnings.size() + scene.warnings.size()) + " warnings";
            row.revision = ++this->revision;
            row.width = map.width;
            row.height = map.height;
            row.border = map.border;
            row.playableX0 = scene.playable[0];
            row.playableY0 = scene.playable[1];
            row.playableX1 = scene.playable[2];
            row.playableY1 = scene.playable[3];
            row.timeOfDay = map.lighting.timeOfDay;
            const auto time = std::size_t(std::clamp(map.lighting.timeOfDay, 1, 4) - 1);
            std::vector<float> lighting;
            for(const auto *light: {&map.lighting.terrain[time], &map.lighting.objects[time]}) {
                lighting.insert(lighting.end(), light->ambient.begin(), light->ambient.end());
                lighting.insert(lighting.end(), light->diffuse.begin(), light->diffuse.end());
                lighting.insert(lighting.end(), light->direction.begin(), light->direction.end());
            }
            if(!map.hasLighting) {  // no GlobalLighting chunk: a plain afternoon sun
                lighting = {0.4f, 0.4f, 0.4f, 0.8f, 0.8f, 0.8f, -0.8f, 0.4f, -0.45f,
                            0.4f, 0.4f, 0.4f, 0.8f, 0.8f, 0.8f, -0.8f, 0.4f, -0.45f};
            }
            row.lighting = toBlob(lighting);
            row.heights = toBlob(map.heights);
            row.objectCount = int(scene.objects.size());
            row.modelCount = int(modelIds.size());
            row.modelRevision = row.revision;
            const auto &look = scene.waterLook;
            row.waterR = look.standingColor[0] * look.diffuse[0];
            row.waterG = look.standingColor[1] * look.diffuse[1];
            row.waterB = look.standingColor[2] * look.diffuse[2];
            row.waterA = look.transparentDiffuse[3];
            row.waterRepeat = look.repeatCount;
            this->storage->replace(row);
            return true;
        });
        if(!this->mapModels.empty()) {
            this->jobs.push_back(Job{Job::loadMapModels, {}});
        }
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
        if(this->nextMapModel < this->mapModels.size()) {
            this->jobs.push_back(Job{Job::loadMapModels, {}});
        }
    }
}
