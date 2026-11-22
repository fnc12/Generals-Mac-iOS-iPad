#pragma once

#include "storage.h"

#include <deque>
#include <memory>
#include <string>
#include <string_view>

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
        struct OpenGameJob {
            std::string gameDir;
        };

        void setStatus(const std::string &state, const std::string &message, const std::string &gameDir,
                       std::int64_t objectCount);
        void openGame(const OpenGameJob &job);

        std::string path;
        std::unique_ptr<Storage> storage;
        std::deque<OpenGameJob> jobs;
        std::int64_t revision = 0;
    };
}
