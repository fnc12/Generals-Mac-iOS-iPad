#include "editor_core.h"

#include "game_data.h"

#include <algorithm>
#include <cstring>

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
        this->setStatus("idle", "", "", 0);
        this->setModelState("", "idle", "");
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
        return "error:unknown_command";
    }

    void EditorCore::update() {
        if(this->jobs.empty()) {
            return;
        }
        const auto job = std::move(this->jobs.front());
        this->jobs.pop_front();
        // A later model or game request makes this one pointless (the user clicked on).
        const auto supersededBy = [&](auto predicate) {
            for(const auto &later: this->jobs) {
                if(predicate(later.kind)) {
                    return true;
                }
            }
            return false;
        };
        switch(job.kind) {
            case Job::openGame:
                this->runOpenGame(job);
                break;
            case Job::openModel:
                if(!supersededBy([](auto kind) { return kind != Job::playAnimation; })) {
                    this->runOpenModel(job.arguments.front());
                }
                break;
            case Job::playAnimation:
                if(!supersededBy([](auto) { return true; })) {
                    this->runPlayAnimation(job.arguments.front());
                }
                break;
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
                row.sourceFile = object.sourceFile;
                row.line = object.line;
                this->storage->insert(row);
            }
            this->setStatus("ready",
                            std::to_string(scan.objects.size()) + " objects from " +
                                std::to_string(scan.iniFileCount) + " INI files in " +
                                std::to_string(scan.archiveCount) + " archives, " +
                                std::to_string(scan.warnings.size()) + " warnings",
                            gameDir,
                            std::int64_t(scan.objects.size()));
            return true;
        });
        if(scan.error.empty()) {
            this->loader = std::make_unique<ModelLoader>(this->files);
        }
    }

    void EditorCore::runOpenModel(const std::string &name) {
        this->model = this->loader->load(name);
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
}
