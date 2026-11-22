#include "game_data.h"

#include "big_archive.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace end_editor {

    bool isObjectIniPath(const std::string &path) {
        const auto endsWith = [&](std::string_view suffix) {
            return path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        return endsWith(".ini") && (path.rfind("data/ini/object/", 0) == 0 || path == "data/ini/default/object.ini");
    }

    GameDataScan scanGameData(const std::filesystem::path &gameDir) {
        GameDataScan scan;
        std::error_code ec;
        if(!std::filesystem::is_directory(gameDir, ec)) {
            scan.error = "not a folder: " + gameDir.string();
            return scan;
        }

        // Where each INI comes from: an archive entry or a loose file.
        struct Source {
            const BigArchive *archive = nullptr;
            BigEntry entry;
            std::filesystem::path looseFile;
        };
        std::map<std::string, Source> sources;

        // Loose files first: they win.
        const auto looseRoot = gameDir / "Data" / "INI";
        if(std::filesystem::is_directory(looseRoot, ec)) {
            for(auto it = std::filesystem::recursive_directory_iterator(looseRoot, ec);
                !ec && it != std::filesystem::recursive_directory_iterator();
                it.increment(ec)) {
                if(!it->is_regular_file(ec)) {
                    continue;
                }
                const auto relative = std::filesystem::relative(it->path(), gameDir, ec).generic_string();
                const auto path = normalizeArchivePath(relative);
                if(isObjectIniPath(path)) {
                    sources.emplace(path, Source{nullptr, {}, it->path()});
                }
            }
        }

        std::vector<std::filesystem::path> archivePaths;
        for(const auto &item: std::filesystem::directory_iterator(gameDir, ec)) {
            if(item.is_regular_file(ec) && normalizeArchivePath(item.path().extension().string()) == ".big") {
                archivePaths.push_back(item.path());
            }
        }
        std::sort(archivePaths.begin(), archivePaths.end(), [](const auto &a, const auto &b) {
            return normalizeArchivePath(a.filename().string()) < normalizeArchivePath(b.filename().string());
        });

        std::vector<BigArchive> archives;
        archives.reserve(archivePaths.size());  // `sources` keeps pointers into this vector
        for(const auto &path: archivePaths) {
            std::string error;
            auto archive = BigArchive::open(path, error);
            if(!archive) {
                scan.warnings.push_back(path.filename().string() + ": " + error);
                continue;
            }
            archives.push_back(std::move(*archive));
        }
        scan.archiveCount = int(archives.size());
        for(const auto &archive: archives) {
            for(const auto &entry: archive.entries()) {
                if(isObjectIniPath(entry.path)) {
                    sources.emplace(entry.path, Source{&archive, entry, {}});
                }
            }
        }

        if(archives.empty() && sources.empty()) {
            scan.error = "no .big archives or Data/INI/Object files in " + gameDir.string();
            return scan;
        }

        for(const auto &[path, source]: sources) {
            std::string text;
            bool ok = false;
            if(source.archive) {
                ok = source.archive->read(source.entry, text);
            } else {
                std::ifstream file(source.looseFile, std::ios::binary);
                std::ostringstream stream;
                stream << file.rdbuf();
                text = stream.str();
                ok = file.is_open() && !file.bad();
            }
            if(!ok) {
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
}
