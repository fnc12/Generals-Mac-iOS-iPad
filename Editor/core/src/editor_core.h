#pragma once

#include "game_files.h"
#include "ini_objects.h"
#include "map_file.h"
#include "model_loader.h"
#include "storage.h"

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace end_editor {

    // The editor logic behind the C ABI (editor_core_c.h). Not thread safe: the client calls it
    // from one thread.
    class EditorCore {
      public:
        // Opens (creates) the store and sets status to "idle". Throws std::system_error on failure.
        explicit EditorCore(std::string dbPath);

        const std::string &dbPath() const {
            return this->path;
        }

        // Returns "" when the command is accepted, "error:<code>" otherwise.
        std::string send(std::string_view command);

        // Runs queued work.
        void update();

        bool hasPendingWork() const {
            return !this->jobs.empty();
        }
        std::size_t pendingJobCount() const {
            return this->jobs.size();
        }

      private:
        struct Job {
            enum Kind { openGame, openModel, playAnimation, openMap, loadMapModels } kind = openGame;
            std::vector<std::string> arguments;
        };

        void setStatus(const std::string &state, const std::string &message, const std::string &gameDir,
                       std::int64_t objectCount);
        void setModelState(const std::string &name, const std::string &state, const std::string &message);
        void runOpenGame(const Job &job);
        void runOpenModel(const std::vector<std::string> &arguments);
        void runPlayAnimation(const std::string &name);
        void setMapState(const std::string &path, const std::string &state, const std::string &message);
        void clearMap();
        void listMaps();
        void runOpenMap(const std::string &path);
        void runLoadMapModels();
        const ObjectInfo *findObject(const std::string &templateName) const;

        std::string path;
        std::unique_ptr<Storage> storage;
        std::deque<Job> jobs;
        std::int64_t revision = 0;
        GameFiles files;
        std::unique_ptr<ModelLoader> loader;
        LoadedModel model;

        std::vector<ObjectInfo> objectInfos;
        std::map<std::string, std::size_t> objectsByName;  // lower-case template name -> objectInfos
        // Models of the open map still to load, in map_models id order.
        struct PendingMapModel {
            std::int64_t id = 0;
            std::string request;  // model \t muzzle flashes \t hidden sub-objects
        };
        std::vector<PendingMapModel> mapModels;
        std::size_t nextMapModel = 0;
        std::set<std::string> mapTextures;  // names already in map_model_textures
    };
}
