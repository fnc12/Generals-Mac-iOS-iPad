#include "game_data.h"

namespace end_editor {

    bool isObjectIniPath(const std::string &path) {
        const auto endsWith = [&](std::string_view suffix) {
            return path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        return endsWith(".ini") && (path.rfind("data/ini/object/", 0) == 0 || path == "data/ini/default/object.ini");
    }

    GameDataScan scanObjects(const GameFiles &files) {
        GameDataScan scan;
        scan.archiveCount = files.archiveCount();
        const auto paths = files.list(isObjectIniPath);
        if(paths.empty()) {
            scan.error = "no object INI files (Data/INI/Object) in the game folder";
            return scan;
        }
        for(const auto &path: paths) {
            std::string text;
            if(!files.read(path, text)) {
                scan.warnings.push_back(path + ": cannot read");
                continue;
            }
            ++scan.iniFileCount;
            auto parsed = parseObjects(text, path);
            scan.warnings.insert(scan.warnings.end(), parsed.warnings.begin(), parsed.warnings.end());
            for(auto &object: parsed.objects) {
                scan.objects.push_back(std::move(object));
            }
        }
        resolveInheritance(scan.objects);
        return scan;
    }

    GameDataScan scanGameData(const std::vector<std::filesystem::path> &folders, GameFiles &files) {
        std::vector<std::string> warnings;
        std::string error;
        if(!files.mount(folders, warnings, error)) {
            GameDataScan scan;
            scan.error = error;
            scan.warnings = std::move(warnings);
            return scan;
        }
        auto scan = scanObjects(files);
        scan.warnings.insert(scan.warnings.begin(), warnings.begin(), warnings.end());
        return scan;
    }

    GameDataScan scanGameData(const std::filesystem::path &gameDir) {
        GameFiles files;
        return scanGameData(std::vector<std::filesystem::path>{gameDir}, files);
    }
}
