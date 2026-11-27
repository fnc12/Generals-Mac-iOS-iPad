#include "end_editor/editor_core_c.h"

#include "editor_core.h"

#include <exception>
#include <filesystem>
#include <memory>
#include <string>

namespace {
    std::unique_ptr<end_editor::EditorCore> core;
    std::string lastResult;
}

extern "C" {

const char* end_editor_get_version(void) {
    return "0.1.0";
}

bool end_editor_initialize(const char* db_path) {
    if (core) {
        return false;
    }
    std::string path = db_path ? db_path : "";
    try {
        if (path.empty()) {
            path = (std::filesystem::temp_directory_path() / "end-editor.sqlite").string();
        }
        core = std::make_unique<end_editor::EditorCore>(path);
        return true;
    } catch (const std::exception&) {
        core.reset();
        return false;
    }
}

void end_editor_shutdown(void) {
    core.reset();
}

const char* end_editor_db_path(void) {
    return core ? core->dbPath().c_str() : "";
}

const char* end_editor_send(const char* command) {
    if (!core) {
        lastResult = "error:not_initialized";
    } else if (!command) {
        lastResult = "error:null_command";
    } else {
        try {
            lastResult = core->send(command);
        } catch (const std::exception&) {
            lastResult = "error:exception";
        }
    }
    return lastResult.c_str();
}

void end_editor_update(void) {
    if (!core) {
        return;
    }
    try {
        core->update();
    } catch (const std::exception&) {
        // The store could not be written; the status row keeps the last state the client saw.
    }
}

int end_editor_pending_work(void) {
    return core ? int(core->pendingJobCount()) : 0;
}
}
