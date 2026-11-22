#include "editor_core.h"

#include "game_data.h"

namespace end_editor {

    EditorCore::EditorCore(std::string dbPath) : path(std::move(dbPath)) {
        this->storage = std::make_unique<Storage>(makeStorage(this->path));
        this->storage->open_forever();
        this->storage->pragma.journal_mode(sqlite_orm::journal_mode::WAL);
        this->storage->sync_schema();
        if(auto previous = this->storage->get_pointer<StatusRow>(1)) {
            this->revision = previous->revision;
        }
        this->setStatus("idle", "", "", 0);
    }

    std::string EditorCore::send(std::string_view command) {
        const auto tab = command.find('\t');
        const auto name = command.substr(0, tab);
        const auto argument = tab == std::string_view::npos ? std::string_view() : command.substr(tab + 1);

        if(name == "game.open") {
            if(argument.empty()) {
                return "error:missing_argument";
            }
            OpenGameJob job{std::string(argument)};
            // Status changes now so the client shows "loading" before the work in update() starts.
            this->setStatus("loading", "Reading game data", job.gameDir, 0);
            this->jobs.push_back(std::move(job));
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
        this->openGame(job);
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

    void EditorCore::openGame(const OpenGameJob &job) {
        const auto scan = scanGameData(job.gameDir);
        this->storage->transaction([&] {
            this->storage->remove_all<ObjectRow>();
            this->storage->remove_all<WarningRow>();
            for(const auto &warning: scan.warnings) {
                this->storage->insert(WarningRow{0, warning});
            }
            if(!scan.error.empty()) {
                this->setStatus("error", scan.error, job.gameDir, 0);
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
                            job.gameDir,
                            std::int64_t(scan.objects.size()));
            return true;
        });
    }
}
