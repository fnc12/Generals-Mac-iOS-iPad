// Headless harness: opens a game folder through the C ABI, the way a client does, and prints what
// the store holds.
//   end-editor-cli <game folder> [db path]
#include "end_editor/editor_core_c.h"

#include <sqlite3.h>

#include <cstdio>
#include <string>

int main(int argc, char **argv) {
    if(argc < 2) {
        std::fprintf(stderr, "usage: %s <game folder> [db path]\n", argv[0]);
        return 2;
    }
    if(!end_editor_initialize(argc > 2 ? argv[2] : nullptr)) {
        std::fprintf(stderr, "cannot open the store\n");
        return 1;
    }
    const std::string command = std::string("game.open\t") + argv[1];
    const std::string result = end_editor_send(command.c_str());
    if(!result.empty()) {
        std::fprintf(stderr, "send: %s\n", result.c_str());
        return 1;
    }
    end_editor_update();
    const std::string dbPath = end_editor_db_path();
    end_editor_shutdown();

    // Read the store like a client would, with its own connection.
    sqlite3 *db = nullptr;
    if(sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::fprintf(stderr, "cannot read %s\n", dbPath.c_str());
        return 1;
    }
    auto print = [db](const char *sql) {
        sqlite3_stmt *statement = nullptr;
        sqlite3_prepare_v2(db, sql, -1, &statement, nullptr);
        while(sqlite3_step(statement) == SQLITE_ROW) {
            const int columns = sqlite3_column_count(statement);
            for(int i = 0; i < columns; ++i) {
                const auto *text = sqlite3_column_text(statement, i);
                std::printf("%s%s", i ? " | " : "", text ? reinterpret_cast<const char *>(text) : "");
            }
            std::printf("\n");
        }
        sqlite3_finalize(statement);
    };
    std::printf("store: %s\n", dbPath.c_str());
    print("SELECT state, message FROM status");
    std::printf("-- objects per side\n");
    print("SELECT side, COUNT(*) FROM objects GROUP BY side ORDER BY COUNT(*) DESC LIMIT 20");
    std::printf("-- objects without a model: ");
    print("SELECT COUNT(*) FROM objects WHERE model = ''");
    std::printf("-- warnings (first 20)\n");
    print("SELECT text FROM warnings LIMIT 20");
    sqlite3_close(db);
    return 0;
}
