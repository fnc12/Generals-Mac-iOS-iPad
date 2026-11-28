#include "editor_core.h"

#include "game_data.h"
#include "game_launch.h"
#include "map_preview.h"
#include "map_scene.h"
#include "overloaded.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <thread>

namespace end_editor {

    namespace {
        bool isInside(const std::filesystem::path& path, const std::filesystem::path& folder) {
            std::error_code error;
            const auto full = std::filesystem::weakly_canonical(path, error);
            const auto base = std::filesystem::weakly_canonical(folder, error);
            const auto relative = full.lexically_relative(base);
            return !base.empty() && !relative.empty() && *relative.begin() != "..";
        }

        template<class T>
        std::vector<char> toBlob(const std::vector<T>& items) {
            std::vector<char> blob(items.size() * sizeof(T));
            if (!blob.empty()) {
                std::memcpy(blob.data(), items.data(), blob.size());
            }
            return blob;
        }
    }

    EditorCore::EditorCore(std::string dbPath) :
        path(std::move(dbPath)), editor(MapEditor::Game{[this](const std::string& name) {
                                                            return this->roadKind(name);
                                                        },
                                                        [this](bool bridge) {
                                                            for (const auto& type: this->roadTypes) {
                                                                if (type.bridge == bridge) {
                                                                    return type.name;
                                                                }
                                                            }
                                                            return std::string();
                                                        }}) {
        this->storage = std::make_unique<Storage>(makeStorage(this->path));
        this->storage->open_forever();
        this->storage->pragma.journal_mode(sqlite_orm::journal_mode::WAL);
        // A view-model store: losing the last commits on a power cut is harmless, and every mouse
        // move of a gesture is a commit.
        this->storage->pragma.synchronous(1);  // NORMAL
        this->storage->sync_schema();
        if (auto previous = this->storage->get_pointer<StatusRow>(1)) {
            this->revision = previous->revision;
        }
        // Only the revisions of the last session's model and map rows, not their blobs.
        using namespace sqlite_orm;
        for (const auto modelRevision:
             this->storage->select(&ModelRow::revision, where(c(&ModelRow::id) == 1), limit(1))) {
            this->revision = std::max(this->revision, modelRevision);
        }
        for (const auto& [mapRevision, modelRevision]:
             this->storage->select(columns(&MapRow::revision, &MapRow::modelRevision),
                                   where(c(&MapRow::id) == 1),
                                   limit(1))) {
            this->revision = std::max({this->revision, mapRevision, modelRevision});
        }
        this->setStatus("idle", "", "", 0);
        this->setModelState("", "idle", "");
        this->storage->transaction([&] {
            this->storage->remove_all<MapListRow>();
            this->storage->remove_all<RoadTypeRow>();
            this->clearMap();
            this->setMapState("", "idle", "");
            if (auto previous = this->storage->get_pointer<MapEditorRow>(1)) {
                this->revision = std::max(this->revision, previous->revision);
            }
            this->publishEditor();
            this->storage->remove_all<FactionRow>();
            this->storage->remove_all<PlayerColorRow>();
            // The Play settings stay from the last session; a game it started then is not ours.
            if (auto previous = this->storage->get_pointer<PlayRow>(1)) {
                this->play = *previous;
            }
            this->play.state = "idle";
            this->play.message.clear();
            this->play.pid = 0;
            this->writePlay();
            return true;
        });
    }

    bool EditorCore::NoCaseLess::operator()(std::string_view a, std::string_view b) const {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
        });
    }

    std::string EditorCore::send(std::string_view command) {
        auto parsed = commands::parse(command);
        if (const auto* error = std::get_if<commands::Error>(&parsed)) {
            // The map editor's commands tell the user why too.
            return error->note.empty() ? error->code : this->refuse(error->code, error->note);
        }
        return std::visit(
            [this](auto&& accepted) {
                return this->accept(std::move(accepted));
            },
            std::get<commands::Command>(std::move(parsed)));
    }

    std::string EditorCore::accept(commands::OpenGame&& command) {
        // Status changes now so the client shows "loading" before the work in update() starts.
        this->setStatus("loading", "Reading game data", command.folder, 0);
        this->jobs.push_back(std::move(command));
        return "";
    }

    std::string EditorCore::accept(commands::OpenModel&& command) {
        if (!this->loader) {
            return "error:no_game";
        }
        this->setModelState(command.name, "loading", "");
        this->jobs.push_back(std::move(command));
        return "";
    }

    std::string EditorCore::accept(commands::PlayAnimation&& command) {
        if (!this->loader) {
            return "error:no_game";
        }
        this->jobs.push_back(std::move(command));
        return "";
    }

    std::string EditorCore::accept(commands::OpenMap&& command) {
        if (!this->loader) {
            return "error:no_game";
        }
        this->setMapState(command.path, "loading", "Reading the map");
        this->jobs.push_back(std::move(command));
        return "";
    }

    std::string EditorCore::accept(commands::SetUserMaps&& command) {
        this->userMapsFolder = std::move(command.folder);
        this->writePlay();  // the default game app follows the game of the Maps folder
        if (this->loader) {
            this->jobs.push_back(ListMaps{});
        }
        return "";
    }

    bool EditorCore::hasMap() const {
        return this->document || this->queued<commands::OpenMap>();
    }

    std::string EditorCore::accept(commands::SaveMap&& command) {
        if (!this->hasMap()) {
            return "error:no_map";
        }
        this->queueBeforeModelLoading(SaveMapTo{std::move(command.file)});
        return "";
    }

    std::string EditorCore::accept(commands::SaveMapAs&& command) {
        if (!this->hasMap()) {
            return this->refuse("error:no_map", "No map is open");
        }
        if (this->userMapsFolder.empty()) {
            return this->refuse("error:no_user_maps", "No user Maps folder to save into (maps.user)");
        }
        const auto target = std::filesystem::path(this->userMapsFolder) / command.name / (command.name + ".map");
        this->queueBeforeModelLoading(SaveMapTo{target.string(), true, command.replace});
        return "";
    }

    std::string EditorCore::accept(commands::PlayMap&& command) {
        if (!this->hasMap()) {
            return this->refuse("error:no_map", "No map is open");
        }
        if (this->userMapsFolder.empty()) {
            return this->refuse("error:no_user_maps", "Play needs the game's user Maps folder (maps.user)");
        }
        this->queueBeforeModelLoading(command);
        return "";
    }

    std::string EditorCore::accept(commands::PlaySetting&& command) {
        return this->setPlaySetting(command);
    }

    std::string EditorCore::accept(commands::Edit&& command) {
        if (commands::needsMap(command) && !this->hasMap()) {
            return this->refuse("error:no_map", "No map is open");
        }
        if (auto error = this->checkNames(command); !error.empty()) {
            return error;
        }
        this->queueBeforeModelLoading(EditMap{std::move(command)});
        return "";
    }

    std::string EditorCore::refuse(const std::string& error, const std::string& note) {
        this->editor.setNote(note, "error");
        this->storage->transaction([&] {
            this->publishEditor();
            return true;
        });
        return error;
    }

    MapEditor::RoadKind EditorCore::roadKind(const std::string& name) const {
        for (const auto& type: this->roadTypes) {
            if (type.name == name) {
                return type.bridge ? MapEditor::RoadKind::bridge : MapEditor::RoadKind::road;
            }
        }
        return MapEditor::RoadKind::unknown;
    }

    std::string EditorCore::checkNames(const commands::Edit& edit) {
        const auto checkTemplate = [this](const std::string& name) {
            return this->findObject(name)
                       ? std::string()
                       : this->refuse("error:unknown_template", "No object " + name + " in this game");
        };
        return std::visit(Overloaded{[&](const commands::PlaceObject& place) {
                                         return checkTemplate(place.templateName);
                                     },
                                     [&](const commands::ChooseTemplate& choose) {
                                         return checkTemplate(choose.templateName);
                                     },
                                     [this](const commands::UseRoadTool& tool) {
                                         const auto kind =
                                             tool.bridge ? MapEditor::RoadKind::bridge : MapEditor::RoadKind::road;
                                         if (tool.type.empty() || this->roadKind(tool.type) == kind) {
                                             return std::string();
                                         }
                                         return this->refuse("error:unknown_road",
                                                             std::string("No ") + (tool.bridge ? "bridge " : "road ") +
                                                                 tool.type + " in Roads.ini");
                                     },
                                     [](const auto&) {
                                         return std::string();
                                     }},
                          edit);
    }

    void EditorCore::queueBeforeModelLoading(Job job) {
        // Model loading re-queues itself at the back; an edit or a save goes before it so it runs
        // in the next update and the client sees it at once.
        auto at = this->jobs.end();
        while (at != this->jobs.begin() && std::holds_alternative<LoadMapModels>(*std::prev(at))) {
            --at;
        }
        this->jobs.insert(at, std::move(job));
    }

    void EditorCore::update() {
        // A later request can make a queued one pointless (the user clicked on); those are
        // dropped without using up the update.
        using commands::OpenGame, commands::OpenModel, commands::PlayAnimation, commands::OpenMap;
        this->pollPlay();
        while (!this->jobs.empty()) {
            auto job = std::move(this->jobs.front());
            this->jobs.pop_front();
            const bool ran = std::visit(Overloaded{[this](const OpenGame& open) {
                                                       this->runOpenGame(open);
                                                       return true;
                                                   },
                                                   [this](const OpenModel& open) {
                                                       if (this->queued<OpenModel, OpenGame>()) {
                                                           return false;
                                                       }
                                                       this->runOpenModel(open);
                                                       return true;
                                                   },
                                                   [this](const PlayAnimation& play) {
                                                       if (this->queued<PlayAnimation, OpenModel, OpenGame>()) {
                                                           return false;
                                                       }
                                                       this->runPlayAnimation(play.name);
                                                       return true;
                                                   },
                                                   [this](const OpenMap& open) {
                                                       if (this->queued<OpenMap, OpenGame>()) {
                                                           return false;
                                                       }
                                                       this->runOpenMap(open.path);
                                                       return true;
                                                   },
                                                   [this](const LoadMapModels&) {
                                                       if (this->queued<OpenMap, OpenGame>()) {
                                                           return false;
                                                       }
                                                       this->runLoadMapModels();
                                                       return true;
                                                   },
                                                   [this](const EditMap& edit) {
                                                       // Edits are quick: every one queued runs now, in order, in one transaction.
                                                       this->storage->transaction([&] {
                                                           this->runEditMap(edit.edit);
                                                           while (!this->jobs.empty() &&
                                                                  std::holds_alternative<EditMap>(this->jobs.front())) {
                                                               const auto next = std::move(this->jobs.front());
                                                               this->jobs.pop_front();
                                                               this->runEditMap(std::get<EditMap>(next).edit);
                                                           }
                                                           return true;
                                                       });
                                                       return true;
                                                   },
                                                   [this](const SaveMapTo& save) {
                                                       this->runSaveMap(save);
                                                       return true;
                                                   },
                                                   [this](const commands::PlayMap&) {
                                                       this->runPlayMap();
                                                       return true;
                                                   },
                                                   [this](const ListMaps&) {
                                                       this->relistMaps();
                                                       return true;
                                                   }},
                                        job);
            if (ran) {
                return;
            }
        }
    }

    void EditorCore::setStatus(const std::string& state,
                               const std::string& message,
                               const std::string& gameDir,
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

    void EditorCore::setModelState(const std::string& name, const std::string& state, const std::string& message) {
        ModelRow row;
        row.name = name;
        row.state = state;
        row.message = message;
        row.revision = ++this->revision;
        this->storage->replace(row);
    }

    void EditorCore::runOpenGame(const commands::OpenGame& command) {
        std::vector<std::filesystem::path> folders{command.folder};
        if (command.baseFolder) {
            folders.emplace_back(*command.baseFolder);
        }
        this->gameFolders = folders;
        this->loader.reset();
        this->model = LoadedModel{};
        auto scan = scanGameData(folders, this->files);
        const auto& gameDir = command.folder;
        this->storage->transaction([&] {
            this->storage->remove_all<ObjectRow>();
            this->storage->remove_all<WarningRow>();
            this->storage->remove_all<ModelPartRow>();
            this->storage->remove_all<ModelBoneRow>();
            this->storage->remove_all<ModelTextureRow>();
            this->storage->remove_all<ModelAnimationRow>();
            this->setModelState("", "idle", "");
            for (const auto& warning: scan.warnings) {
                this->storage->insert(WarningRow{0, warning});
            }
            this->storage->remove_all<MapListRow>();
            this->storage->remove_all<RoadTypeRow>();
            this->roadTypes.clear();
            this->clearMap();
            this->setMapState("", "idle", "");
            if (!scan.error.empty()) {
                this->setStatus("error", scan.error, gameDir, 0);
                return true;
            }
            ObjectRow row;  // one row for every object: its strings keep their buffers
            for (const auto& object: scan.objects) {
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
            this->roadTypes.clear();
            this->files.readEach("data/ini/roads.ini", [&](const std::string& text) {
                for (auto& type: parseRoadTypes(text)) {  // the first copy wins
                    if (this->roadKind(type.name) == MapEditor::RoadKind::unknown) {
                        this->roadTypes.push_back(std::move(type));
                    }
                }
            });
            for (const auto& type: this->roadTypes) {
                this->storage->insert(RoadTypeRow{0, type.name, type.bridge});
            }
            this->storage->remove_all<FactionRow>();
            this->factionNames.clear();
            this->files.readEach("data/ini/playertemplate.ini", [&](const std::string& text) {
                for (auto& faction: parsePlayableFactions(text)) {  // the first copy wins
                    if (std::find(this->factionNames.begin(), this->factionNames.end(), faction.name) ==
                        this->factionNames.end()) {
                        this->factionNames.push_back(faction.name);
                        this->storage->insert(FactionRow{0, faction.name, faction.side});
                    }
                }
            });
            this->storage->remove_all<PlayerColorRow>();
            this->colorNames.clear();
            // The game reads one copy of each, the default first (GameEngine.cpp: TheMultiplayerSettings).
            std::string multiplayer;
            for (const auto* path: {"data/ini/default/multiplayer.ini", "data/ini/multiplayer.ini"}) {
                if (this->files.read(path, multiplayer)) {
                    for (auto& color: parsePlayerColors(multiplayer)) {
                        this->storage->insert(
                            PlayerColorRow{0, int(this->colorNames.size()), color.name, int(color.rgb)});
                        this->colorNames.push_back(std::move(color.name));
                    }
                }
            }
            this->writePlay();  // the default app follows the game
            this->setStatus("ready",
                            std::to_string(scan.objects.size()) + " objects from " + std::to_string(scan.iniFileCount) +
                                " INI files in " + std::to_string(scan.archiveCount) + " archives, " +
                                std::to_string(scan.warnings.size()) + " warnings",
                            gameDir,
                            std::int64_t(scan.objects.size()));
            return true;
        });
        this->objectsByName.clear();  // views of objectInfos' names
        this->objectInfos.clear();
        if (scan.error.empty()) {
            this->loader = std::make_unique<ModelLoader>(this->files);
            this->objectInfos = std::move(scan.objects);
            for (std::size_t i = 0; i < this->objectInfos.size(); ++i) {
                this->objectsByName.emplace(this->objectInfos[i].name, i);
            }
        }
    }

    void EditorCore::runOpenModel(const commands::OpenModel& command) {
        const std::string& name = command.name;
        const ModelLoader::Hiding hiding{command.muzzleFlashes, command.hiddenSubObjects};
        this->model = this->loader->load(name, hiding);
        std::string poseError;
        const auto pose = this->model.error.empty() ? this->loader->pose(this->model, "", poseError) : ModelPose{};
        this->storage->transaction([&] {
            this->storage->remove_all<ModelPartRow>();
            this->storage->remove_all<ModelBoneRow>();
            this->storage->remove_all<ModelTextureRow>();
            this->storage->remove_all<ModelAnimationRow>();
            if (!this->model.error.empty()) {
                this->setModelState(name, "error", this->model.error);
                return true;
            }
            for (const auto& part: this->model.parts) {
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
            const auto& pivots = this->model.hierarchy.pivots;
            for (std::size_t i = 0; i < pivots.size(); ++i) {
                this->storage->insert(ModelBoneRow{0, int(i), pivots[i].name, pivots[i].parent});
            }
            for (const auto& [textureName, image]: this->model.textures) {
                this->storage->insert(
                    ModelTextureRow{0, textureName, int(image.width), int(image.height), toBlob(image.rgba)});
            }
            for (const auto& animation: this->model.animations) {
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

    void EditorCore::runPlayAnimation(const std::string& name) {
        using namespace sqlite_orm;
        // The model row's state and message, not the pose blob that is replaced anyway.
        std::optional<std::string> message;
        for (auto&& [state, text]: this->storage->select(columns(&ModelRow::state, &ModelRow::message),
                                                         where(c(&ModelRow::id) == 1),
                                                         limit(1))) {
            if (state == "ready") {
                message = std::move(text);
            }
        }
        if (!message || !this->model.error.empty() || this->model.parts.empty()) {
            return;
        }
        std::string error;
        const auto pose = this->loader->pose(this->model, name, error);
        this->storage->update_all(set(c(&ModelRow::animation) = error.empty() ? name : std::string(),
                                      c(&ModelRow::message) = error.empty() ? *message : error,
                                      c(&ModelRow::frameCount) = int(pose.frameCount),
                                      c(&ModelRow::frameRate) = int(pose.frameRate),
                                      c(&ModelRow::pose) = toBlob(pose.matrices),
                                      c(&ModelRow::visible) = toBlob(pose.visible),
                                      c(&ModelRow::revision) = ++this->revision),
                                  where(c(&ModelRow::id) == 1));
    }

    void EditorCore::setMapState(const std::string& path, const std::string& state, const std::string& message) {
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
        this->storage->remove_all<MapLinkRow>();
        this->mapModels.clear();
        this->nextMapModel = 0;
        this->mapTextures.clear();
        this->mapModelIds.clear();
        this->mapModelJobQueued = false;
        this->editor.open(nullptr, {}, "", {});
        this->document.reset();
        this->tileColors.clear();
        this->publishEditor();
    }

    void EditorCore::listMaps() {
        MapListRow row;  // one row for every map: its strings keep their buffers
        this->files.forEachPath([&](const std::string& path) {
            if (path.rfind("maps/", 0) != 0 || path.size() <= 4 || path.compare(path.size() - 4, 4, ".map") != 0) {
                return;
            }
            row.path = path;
            const auto original = this->files.originalPath(path);
            // "Maps/Alpine Assault/Alpine Assault.map" -> "Alpine Assault"
            const auto slash = original.rfind('/');
            const auto folderStart = original.rfind('/', slash == std::string::npos || slash == 0 ? 0 : slash - 1);
            if (slash == std::string::npos || folderStart == std::string::npos || folderStart >= slash) {
                row.name = original;
            } else {
                row.name.assign(original, folderStart + 1, slash - folderStart - 1);
            }
            row.source = this->files.sourceOf(path);
            this->storage->insert(row);
        });
        // The game's user maps: Maps/<name>/<name>.map (MapCache::loadMapsFromDisk), opened by
        // their path on disk.
        std::error_code error;
        if (this->userMapsFolder.empty() || !std::filesystem::is_directory(this->userMapsFolder, error)) {
            return;
        }
        std::vector<std::filesystem::path> userMaps;
        for (const auto& folder: std::filesystem::directory_iterator(this->userMapsFolder, error)) {
            if (!folder.is_directory(error) || folder.path().filename() == testMapName) {
                continue;  // the Play button's test copy is not one of the user's maps
            }
            for (const auto& file: std::filesystem::directory_iterator(folder.path(), error)) {
                auto extension = file.path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
                    return char(std::tolower(c));
                });
                if (file.is_regular_file(error) && extension == ".map") {
                    userMaps.push_back(file.path());
                }
            }
        }
        std::sort(userMaps.begin(), userMaps.end());
        for (const auto& file: userMaps) {
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
            using namespace sqlite_orm;
            this->storage->update_all(set(c(&StatusRow::mapListRevision) = ++this->mapListRevision),
                                      where(c(&StatusRow::id) == 1));
            return true;
        });
    }

    const ObjectInfo* EditorCore::findObject(const std::string& templateName) const {
        const auto found = this->objectsByName.find(std::string_view(templateName));
        return found == this->objectsByName.end() ? nullptr : &this->objectInfos[found->second];
    }

    void EditorCore::runOpenMap(const std::string& path) {
        // A game path ("maps/x/x.map") or a .map file anywhere on disk.
        std::string bytes;
        std::string name;
        const auto normalized = normalizeArchivePath(path);
        if (this->files.exists(normalized)) {
            this->files.read(normalized, bytes);
            name = std::filesystem::path(this->files.originalPath(normalized)).stem().string();
        } else {
            std::ifstream file(std::filesystem::path(path), std::ios::binary);
            if (!file) {
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
        auto scene = parsed ? buildMapScene(map, this->files) : MapScene{};
        this->storage->transaction([&] {
            this->clearMap();
            if (!parsed) {
                this->setMapState(path, "error", error);
                return true;
            }
            for (const auto& warning: map.warnings) {
                this->storage->insert(MapWarningRow{0, warning});
            }
            for (const auto& warning: scene.warnings) {
                this->storage->insert(MapWarningRow{0, warning});
            }
            this->storage->insert(
                MapTextureRow{0, "terrain", int(scene.atlas.width), int(scene.atlas.height), toBlob(scene.atlas.rgba)});
            if (scene.waterTexture.width > 0) {
                this->storage->insert(MapTextureRow{0,
                                                    "water",
                                                    int(scene.waterTexture.width),
                                                    int(scene.waterTexture.height),
                                                    toBlob(scene.waterTexture.rgba)});
            }
            for (const auto& chunk: scene.chunks) {
                this->storage->insert(
                    MapTerrainRow{0, chunk.x, chunk.y, toBlob(chunk.vertices), toBlob(chunk.indices)});
            }
            for (const auto& water: scene.water) {
                this->storage->insert(MapWaterRow{0,
                                                  water.name,
                                                  water.river,
                                                  water.height,
                                                  toBlob(water.vertices),
                                                  toBlob(water.indices)});
            }

            const auto players = mapPlayers(map);
            for (const auto& player: players) {
                this->storage->insert(MapPlayerRow{0,
                                                   player.name,
                                                   player.displayName,
                                                   player.team,
                                                   int(player.color),
                                                   player.neutral,
                                                   player.start});
            }
            const auto objectCount = map.objects.size();
            // Before the document takes the objects (the start waypoints give the start positions' teams).
            auto teams = mapTeams(map);
            this->document = std::make_unique<MapDocument>(std::move(map));
            // The document numbers the objects the engine keeps 1.. in file order, like the scene.
            for (const auto& entry: this->document->objects()) {
                if (entry.id != 0) {
                    this->writeMapObject(entry.id);
                }
            }
            const auto& opened = this->document->map();
            this->tileColors = std::move(scene.tileColors);
            this->writeMapLinks();
            this->editor.open(this->document.get(),
                              std::move(teams),
                              players.empty() ? std::string() : players.front().team,
                              MapEditor::Point{(scene.playable[0] + scene.playable[2]) / 2,
                                               (scene.playable[1] + scene.playable[3]) / 2});
            this->publishEditor();

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
            lighting.reserve(18);
            for (const auto* light: {&opened.lighting.terrain[time], &opened.lighting.objects[time]}) {
                lighting.insert(lighting.end(), light->ambient.begin(), light->ambient.end());
                lighting.insert(lighting.end(), light->diffuse.begin(), light->diffuse.end());
                lighting.insert(lighting.end(), light->direction.begin(), light->direction.end());
            }
            if (!opened.hasLighting) {  // no GlobalLighting chunk: a plain afternoon sun
                lighting = {0.4f,
                            0.4f,
                            0.4f,
                            0.8f,
                            0.8f,
                            0.8f,
                            -0.8f,
                            0.4f,
                            -0.45f,
                            0.4f,
                            0.4f,
                            0.4f,
                            0.8f,
                            0.8f,
                            0.8f,
                            -0.8f,
                            0.4f,
                            -0.45f};
            }
            row.lighting = toBlob(lighting);
            row.heights = toBlob(opened.heights);
            row.objectCount = int(objectCount);
            row.modelCount = int(this->mapModelIds.size());
            row.modelRevision = row.revision;
            row.objectRevision = row.revision;
            const auto& look = scene.waterLook;
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
            while (this->nextMapModel < this->mapModels.size() &&
                   (loaded == 0 || std::chrono::steady_clock::now() - start < budget)) {
                const auto& pending = this->mapModels[this->nextMapModel++];
                ++loaded;
                auto model = this->loader->load(pending.model, pending.hiding, false);
                MapModelRow row;
                row.id = pending.id;
                row.name = pending.model;
                if (!model.error.empty()) {
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
                for (auto& part: model.parts) {
                    const bool boneHidden =
                        !part.skinned && part.bone < pose.visible.size() && pose.visible[part.bone] == 0;
                    if (part.hidden || boneHidden || part.vertices.empty()) {
                        continue;
                    }
                    // Into the bind pose: the map draws every model standing still.
                    for (auto& vertex: part.vertices) {
                        const auto bone =
                            vertex.bone < boneCount && vertex.bone < pose.matrices.size() ? vertex.bone : 0;
                        if (bone < pose.matrices.size()) {
                            const auto& m = pose.matrices[bone];
                            const float p[3] = {vertex.position[0], vertex.position[1], vertex.position[2]};
                            const float n[3] = {vertex.normal[0], vertex.normal[1], vertex.normal[2]};
                            for (int i = 0; i < 3; ++i) {
                                vertex.position[i] = m[std::size_t(i)] * p[0] + m[std::size_t(4 + i)] * p[1] +
                                                     m[std::size_t(8 + i)] * p[2] + m[std::size_t(12 + i)];
                                vertex.normal[i] = m[std::size_t(i)] * n[0] + m[std::size_t(4 + i)] * n[1] +
                                                   m[std::size_t(8 + i)] * n[2];
                            }
                        }
                        vertex.bone = 0;
                        for (int i = 0; i < 3; ++i) {
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
                    if (!part.texture.empty() && !this->mapTextures.count(part.texture)) {
                        const auto found = model.textures.find(part.texture);
                        if (found != model.textures.end()) {
                            this->mapTextures.insert(part.texture);
                            // The texture goes once (mapTextures): the model's copy is not needed again.
                            auto image = shrinkImage(std::move(found->second), 512);
                            this->storage->insert(MapModelTextureRow{0,
                                                                     part.texture,
                                                                     int(image.width),
                                                                     int(image.height),
                                                                     toBlob(image.rgba)});
                        }
                    }
                }
                row.state = parts > 0 ? "ready" : "error";
                row.message = parts > 0 ? std::to_string(parts) + " parts" : "nothing visible";
                if (parts > 0) {
                    row.minX = low[0];
                    row.minY = low[1];
                    row.minZ = low[2];
                    row.maxX = high[0];
                    row.maxY = high[1];
                    row.maxZ = high[2];
                }
                this->storage->replace(row);
            }
            // Two numbers of the map row: its blobs are not read and written again.
            using namespace sqlite_orm;
            this->storage->update_all(
                set(c(&MapRow::modelsLoaded) = int(this->nextMapModel), c(&MapRow::modelRevision) = ++this->revision),
                where(c(&MapRow::id) == 1));
            return true;
        });
        this->mapModelJobQueued = this->nextMapModel < this->mapModels.size();
        if (this->mapModelJobQueued) {
            this->jobs.push_back(LoadMapModels{});
        }
    }

    void EditorCore::writeMapObject(std::int64_t id) {
        using namespace sqlite_orm;
        this->storage->remove_all<MapPropertyRow>(where(c(&MapPropertyRow::objectId) == id));
        const auto* entry = this->document ? this->document->find(id) : nullptr;
        if (!entry) {
            this->storage->remove<MapObjectRow>(id);
            return;
        }
        const auto placed = resolveObject(this->document->map(), entry->object, [&](const std::string& name) {
            return this->findObject(name);
        });
        MapObjectRow row;
        row.id = id;
        row.templateName = placed.templateName;
        row.kind = placed.kind;
        row.x = placed.x;
        row.y = placed.y;
        row.z = placed.z;
        row.height = entry->object.z;
        row.angle = placed.angle;
        row.flags = placed.flags;
        row.owner = placed.owner;
        row.color = int(placed.color);
        row.start = placed.start;
        if (!placed.model.empty()) {
            // One map_models row per distinct model and hidden parts, in first-use order.
            auto [it, added] =
                this->mapModelIds.emplace(ModelKey{placed.model, placed.muzzleFlashes, placed.hiddenSubObjects},
                                          std::int64_t(this->mapModelIds.size() + 1));
            if (added) {
                MapModelRow model;
                model.id = it->second;
                model.name = placed.model;
                model.state = "pending";
                this->storage->replace(model);
                this->mapModels.push_back(
                    PendingMapModel{it->second,
                                    placed.model,
                                    ModelLoader::Hiding{placed.muzzleFlashes, placed.hiddenSubObjects}});
                if (!this->mapModelJobQueued && this->loader) {
                    this->jobs.push_back(LoadMapModels{});
                    this->mapModelJobQueued = true;
                }
            }
            row.modelId = it->second;
        }
        this->storage->replace(row);
        for (const auto& value: entry->object.properties) {
            this->storage->insert(MapPropertyRow{0, id, value.key, dictTypeName(value), value.toString()});
        }
    }

    void EditorCore::updateMapEditState(const std::string& message) {
        if (!this->document) {
            return;
        }
        // The editing columns of the map row only: its blobs are not read and written again.
        using namespace sqlite_orm;
        const auto objectCount = int(
            std::count_if(this->document->objects().begin(), this->document->objects().end(), [](const auto& entry) {
                return entry.id != 0 && !entry.removed;
            }));
        this->storage->update_all(set(c(&MapRow::objectRevision) = ++this->revision,
                                      c(&MapRow::undoCount) = int(this->document->undoCount()),
                                      c(&MapRow::redoCount) = int(this->document->redoCount()),
                                      c(&MapRow::edited) = this->document->edited(),
                                      c(&MapRow::lastObject) = this->document->lastChangedId(),
                                      c(&MapRow::editMessage) = message,
                                      c(&MapRow::modelCount) = int(this->mapModelIds.size()),
                                      c(&MapRow::objectCount) = objectCount),
                                  where(c(&MapRow::id) == 1));
    }

    void EditorCore::runEditMap(const commands::Edit& edit) {
        const auto revision = this->document ? this->document->revision() : -1;
        this->editor.run(edit);
        if (this->document && this->document->revision() != revision) {
            for (const auto id: this->document->lastChanged()) {
                this->writeMapObject(id);
            }
            this->writeMapLinks();
            this->updateMapEditState(this->editor.state().note);
        }
        this->publishEditor();
    }

    void EditorCore::writeMapLinks() {
        this->storage->remove_all<MapLinkRow>();
        if (!this->document) {
            return;
        }
        for (const auto& link: this->document->links()) {
            const auto from = this->document->waypointObject(link[0]), to = this->document->waypointObject(link[1]);
            if (from != 0 && to != 0) {
                this->storage->insert(MapLinkRow{0, "waypoint", from, to});
            }
        }
        for (const auto& entry: this->document->objects()) {
            if (entry.id == 0 || entry.removed ||
                !(entry.object.flags & (mapObjectRoadPoint1 | mapObjectBridgePoint1))) {
                continue;
            }
            if (const auto partner = this->document->roadPartner(entry.id); partner != 0) {
                const bool bridge = entry.object.flags & mapObjectBridgePoint1;
                this->storage->insert(MapLinkRow{0, bridge ? "bridge" : "road", entry.id, partner});
            }
        }
    }

    void EditorCore::publishEditor() {
        const auto& state = this->editor.state();
        if (state == this->published && this->publishedSelection == state.selectionRevision) {
            return;
        }
        if (this->publishedSelection != state.selectionRevision) {
            this->storage->remove_all<MapSelectionRow>();
            for (const auto id: state.selection) {
                this->storage->insert(MapSelectionRow{0, id});
            }
            this->publishedSelection = state.selectionRevision;
        }
        MapEditorRow row;
        row.tool = state.tool;
        row.templateName = state.templateName;
        row.roadType = state.roadType;
        row.team = state.team;
        row.angle = state.angle;
        row.picker = state.picker;
        row.pending = state.pending.has_value();
        if (state.pending) {
            row.pendingX = state.pending->x;
            row.pendingY = state.pending->y;
        }
        row.roadStart = state.roadStart.has_value();
        if (state.roadStart) {
            row.roadX = state.roadStart->x;
            row.roadY = state.roadStart->y;
        }
        row.gesture = state.gesture;
        row.dragX = state.drag.x;
        row.dragY = state.drag.y;
        row.note = state.note;
        row.noteKind = state.noteKind;
        row.noteSerial = state.noteSerial;
        row.selectionRevision = state.selectionRevision;
        row.selectionCount = int(state.selection.size());
        row.clipboardCount = int(state.clipboardCount);
        row.revision = ++this->revision;
        this->storage->replace(row);
        this->published = state;
    }

    std::string EditorCore::writeMapFiles(const std::filesystem::path& target, std::string& previewError) {
        for (const auto& folder: this->gameFolders) {
            if (isInside(target, folder)) {
                return "error: not saved, " + target.string() + " is inside the game folder";
            }
        }
        const auto bytes = this->document->save();
        std::error_code error;
        if (target.has_parent_path()) {
            std::filesystem::create_directories(target.parent_path(), error);
        }
        // Written next to the target, then renamed over it: a failed write leaves no half file.
        auto temporary = target;
        temporary += ".saving";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(bytes.data(), std::streamsize(bytes.size()));
            if (!file) {
                std::filesystem::remove(temporary, error);
                return "error: cannot write " + temporary.string();
            }
        }
        std::filesystem::rename(temporary, target, error);
        if (error) {
            const auto message = "error: cannot write " + target.string() + ": " + error.message();
            std::filesystem::remove(temporary, error);
            return message;
        }
        // The game's map lists show <name>.tga from next to the map (WorldBuilder writes it too).
        auto preview = target;
        preview.replace_extension(".tga");
        const auto image =
            encodeTga(mapPreviewSize, mapPreviewSize, renderMapPreview(this->document->map(), this->tileColors));
        std::ofstream file(preview, std::ios::binary | std::ios::trunc);
        file.write(image.data(), std::streamsize(image.size()));
        if (!file) {
            previewError = "error: saved, but cannot write the preview " + preview.string();
        }
        return "";
    }

    void EditorCore::runSaveMap(const SaveMapTo& save) {
        if (!this->document) {
            return;
        }
        const auto& path = save.path;
        const bool saveAs = save.saveAs;
        const bool replace = save.replace;
        const std::filesystem::path target(path);
        using namespace sqlite_orm;
        std::error_code error;
        // Save As into the Maps folder does not silently replace another map: the store says
        // which file is in the way, and the client asks before sending "replace".
        if (saveAs && !replace && std::filesystem::exists(target, error) &&
            this->storage->count<MapRow>(where(c(&MapRow::id) == 1 and c(&MapRow::savedPath) == path)) == 0) {
            const auto message = "error: " + target.parent_path().filename().string() +
                                 " already exists in the Maps folder; save again to replace it";
            this->storage->transaction([&] {
                this->storage->update_all(set(c(&MapRow::saveConflict) = path), where(c(&MapRow::id) == 1));
                this->updateMapEditState(message);
                this->editor.setNote(message, "error");
                this->publishEditor();
                return true;
            });
            return;
        }
        std::string previewError;
        auto message = this->writeMapFiles(target, previewError);
        this->storage->transaction([&] {
            if (message.empty()) {
                this->document->markSaved();
                this->storage->update_all(set(c(&MapRow::saveConflict) = "", c(&MapRow::savedPath) = path),
                                          where(c(&MapRow::id) == 1));
                message = previewError.empty() ? "Saved " + path : previewError;
            } else {
                this->storage->update_all(set(c(&MapRow::saveConflict) = ""), where(c(&MapRow::id) == 1));
            }
            this->updateMapEditState(message);
            this->editor.setNote(message, message.rfind("error", 0) == 0 ? "error" : "info");
            this->publishEditor();
            return true;
        });
        if (!this->userMapsFolder.empty() && isInside(target, this->userMapsFolder)) {
            this->relistMaps();
        }
    }

    std::string EditorCore::defaultGameApp() const {
        const bool baseGame = !this->userMapsFolder.empty() && isBaseGameUserMaps(this->userMapsFolder);
        return end_editor::defaultGameApp(homeFolder(), baseGame);
    }

    std::optional<int> EditorCore::colorIndex(std::string_view name) const {
        const auto found = std::find(this->colorNames.begin(), this->colorNames.end(), name);
        if (found == this->colorNames.end()) {
            return std::nullopt;
        }
        return int(found - this->colorNames.begin());
    }

    void EditorCore::writePlay() {
        this->play.id = 1;
        this->play.appResolved = this->play.app.empty() ? this->defaultGameApp() : this->play.app;
        this->play.revision = ++this->revision;
        this->storage->replace(this->play);
    }

    std::string EditorCore::setPlaySetting(const commands::PlaySetting& setting) {
        auto error =
            std::visit(Overloaded{[this](const commands::SetPlayApp& app) {
                                      this->play.app = app.app;  // "" goes back to the default
                                      return std::string();
                                  },
                                  [this](const commands::SetPlayFaction& faction) {
                                      const auto& name = faction.faction;
                                      const bool known =
                                          name == "Random" || this->factionNames.empty() ||
                                          std::find(this->factionNames.begin(), this->factionNames.end(), name) !=
                                              this->factionNames.end();
                                      if (!known) {
                                          return this->refuse("error:unknown_faction",
                                                              "No playable faction " + name + " in PlayerTemplate.ini");
                                      }
                                      this->play.faction = name;
                                      return std::string();
                                  },
                                  [this](const commands::SetPlayColor& color) {
                                      const auto& name = color.color;
                                      if (name != "Random" && !this->colorNames.empty() && !this->colorIndex(name)) {
                                          return this->refuse("error:unknown_color",
                                                              "No player color " + name + " in Multiplayer.ini");
                                      }
                                      this->play.color = name;
                                      return std::string();
                                  },
                                  [this](const commands::SetPlayAiCount& ai) {
                                      this->play.aiCount = ai.count;
                                      return std::string();
                                  },
                                  [this](const commands::SetPlayStart& start) {
                                      this->play.startPosition = start.position;
                                      return std::string();
                                  },
                                  [this](const commands::SetPlayDifficulty& difficulty) {
                                      this->play.aiDifficulty = difficulty.difficulty;
                                      return std::string();
                                  },
                                  [this](const commands::StopPlay&) {
                                      if (this->play.state == "running") {
                                          stopProcess(this->play.pid);
                                          this->playStopAsked = std::chrono::steady_clock::now();
                                      }
                                      return std::string();
                                  }},
                       setting);
        if (!error.empty()) {
            return error;
        }
        this->storage->transaction([&] {
            this->writePlay();
            return true;
        });
        return "";
    }

    void EditorCore::pollPlay() {
        if (this->play.state != "running") {
            return;
        }
        const auto code = pollProcess(this->play.pid);
        if (!code) {
            // A game that does not quit when asked (SIGTERM while it loads) is ended.
            if (this->playStopAsked &&
                std::chrono::steady_clock::now() - *this->playStopAsked > std::chrono::seconds(3)) {
                stopProcess(this->play.pid, true);
                this->playStopAsked.reset();
            }
            return;
        }
        this->playStopAsked.reset();
        this->play.state = "exited";
        this->play.exitCode = *code;
        this->play.message = "The game ended (exit code " + std::to_string(*code) + ")";
        this->play.pid = 0;
        this->storage->transaction([&] {
            this->writePlay();
            return true;
        });
    }

    void EditorCore::runPlayMap() {
        if (!this->document) {
            return;
        }
        std::string message;
        const auto target =
            std::filesystem::path(this->userMapsFolder) / testMapName / (std::string(testMapName) + ".map");
        std::string previewError;
        message = this->writeMapFiles(target, previewError);
        const auto app = this->play.app.empty() ? this->defaultGameApp() : this->play.app;
        const auto program = programForApp(app);
        if (message.empty() && program.empty()) {
            message = "error: no game app at " + app + " (choose it in the Play settings)";
        }
        if (message.empty()) {
            // Play again restarts the test: the game the last Play started is closed first.
            if (this->play.state == "running") {
                // Asked to quit; ended for good when it has not after 3 s.
                stopProcess(this->play.pid);
                bool ended = false;
                for (int i = 0; i < 150 && !ended; ++i) {
                    ended = pollProcess(this->play.pid).has_value();
                    if (!ended) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                }
                if (!ended) {
                    stopProcess(this->play.pid, true);
                    for (int i = 0; i < 50 && !pollProcess(this->play.pid); ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                }
                this->playStopAsked.reset();
            }
            LaunchRequest request;
            request.program = program;
            request.arguments = {"-testMap",
                                 target.string(),
                                 "-testMapFaction",
                                 this->play.faction,
                                 "-testMapAI",
                                 std::to_string(this->play.aiCount),
                                 "-testMapAIDifficulty",
                                 this->play.aiDifficulty,
                                 "-testMapStart",
                                 std::to_string(this->play.startPosition)};
            // Random (or a color this game does not have) leaves the color to the game.
            if (const auto color = this->colorIndex(this->play.color)) {
                request.arguments.emplace_back("-testMapColor");
                request.arguments.push_back(std::to_string(*color));
            }
            // The game reads the same data the editor shows.
            if (!this->gameFolders.empty()) {
                request.environment.emplace_back("CNC_GENERALS_ZH_PATH", this->gameFolders.front().string());
            }
            if (this->gameFolders.size() > 1) {
                request.environment.emplace_back("CNC_GENERALS_PATH", this->gameFolders[1].string());
            }
            // Its user data folder is the one whose Maps folder has the test copy (the same one by
            // default; GeneralsX reads GENERALSX_USER_DATA_DIR).
            request.environment.emplace_back("GENERALSX_USER_DATA_DIR",
                                             std::filesystem::path(this->userMapsFolder).parent_path().string());
            request.logPath = (std::filesystem::path(this->path).parent_path() / "play.log").string();
            const auto launched = launchProcess(request);
            std::string command = request.program;
            for (const auto& argument: request.arguments) {
                command.append(1, '\t').append(argument);
            }
            this->play.command = command;
            this->play.mapPath = target.string();
            this->play.logPath = request.logPath;
            if (launched.error.empty()) {
                this->play.state = "running";
                this->play.pid = launched.pid;
                this->play.exitCode = 0;
                ++this->play.runs;
                using namespace sqlite_orm;
                std::string name = "the map";
                for (auto&& mapName: this->storage->select(&MapRow::name, where(c(&MapRow::id) == 1), limit(1))) {
                    name = std::move(mapName);
                }
                message = "Playing " + name + " in " + std::filesystem::path(app).filename().string();
                this->play.message = message;
            } else {
                message = "error: " + launched.error;
            }
        }
        if (message.rfind("error", 0) == 0) {
            this->play.state = "error";
            this->play.message = message;
            this->play.pid = 0;
        }
        this->storage->transaction([&] {
            this->writePlay();
            this->editor.setNote(message, message.rfind("error", 0) == 0 ? "error" : "info");
            this->publishEditor();
            return true;
        });
    }
}
