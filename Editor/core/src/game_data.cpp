#include "game_data.h"

#include <iterator>

namespace end_editor {

    bool isObjectIniPath(const std::string& path) {
        const auto endsWith = [&](std::string_view suffix) {
            return path.size() >= suffix.size() &&
                   path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        return endsWith(".ini") && (path.rfind("data/ini/object/", 0) == 0 || path == "data/ini/default/object.ini");
    }

    GameDataScan scanObjects(const GameFiles& files) {
        GameDataScan scan;
        scan.archiveCount = files.archiveCount();
        bool found = false;
        std::string text;  // one buffer for every file
        files.forEachPath([&](const std::string& path) {
            if (!isObjectIniPath(path)) {
                return;
            }
            found = true;
            if (!files.read(path, text)) {
                scan.warnings.push_back(path + ": cannot read");
                return;
            }
            ++scan.iniFileCount;
            auto parsed = parseObjects(text, path);
            scan.warnings.insert(scan.warnings.end(),
                                 std::make_move_iterator(parsed.warnings.begin()),
                                 std::make_move_iterator(parsed.warnings.end()));
            scan.objects.insert(scan.objects.end(),
                                std::make_move_iterator(parsed.objects.begin()),
                                std::make_move_iterator(parsed.objects.end()));
        });
        if (!found) {
            scan.error = "no object INI files (Data/INI/Object) in the game folder";
            return scan;
        }
        resolveInheritance(scan.objects);
        return scan;
    }

    GameDataScan scanGameData(const std::vector<std::filesystem::path>& folders, GameFiles& files) {
        std::vector<std::string> warnings;
        std::string error;
        if (!files.mount(folders, warnings, error)) {
            GameDataScan scan;
            scan.error = error;
            scan.warnings = std::move(warnings);
            return scan;
        }
        auto scan = scanObjects(files);
        scan.warnings.insert(scan.warnings.begin(),
                             std::make_move_iterator(warnings.begin()),
                             std::make_move_iterator(warnings.end()));
        return scan;
    }

    GameDataScan scanGameData(const std::filesystem::path& gameDir) {
        GameFiles files;
        return scanGameData(std::vector<std::filesystem::path>{gameDir}, files);
    }
}
