#pragma once

#include "game_files.h"
#include "model_loader.h"
#include "storage.h"

#include <deque>
#include <memory>
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

      private:
        struct Job {
            enum Kind { openGame, openModel, playAnimation } kind = openGame;
            std::vector<std::string> arguments;
        };

        void setStatus(const std::string &state, const std::string &message, const std::string &gameDir,
                       std::int64_t objectCount);
        void setModelState(const std::string &name, const std::string &state, const std::string &message);
        void runOpenGame(const Job &job);
        void runOpenModel(const std::string &name);
        void runPlayAnimation(const std::string &name);

        std::string path;
        std::unique_ptr<Storage> storage;
        std::deque<Job> jobs;
        std::int64_t revision = 0;
        GameFiles files;
        std::unique_ptr<ModelLoader> loader;
        LoadedModel model;
    };
}
