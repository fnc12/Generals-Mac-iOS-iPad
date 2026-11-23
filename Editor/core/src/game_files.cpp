#include "game_files.h"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace end_editor {

    bool GameFiles::mount(const std::vector<std::filesystem::path> &folders, std::vector<std::string> &warnings,
                          std::string &error) {
        this->archives.clear();
        this->sources.clear();
        std::error_code ec;
        if(folders.empty() || !std::filesystem::is_directory(folders.front(), ec)) {
            error = "not a folder: " + (folders.empty() ? std::string() : folders.front().string());
            return false;
        }

        for(const auto &folder: folders) {
            if(!std::filesystem::is_directory(folder, ec)) {
                warnings.push_back("not a folder: " + folder.string());
                continue;
            }

            // Loose files first: they win over this folder's archives.
            for(const char *top: {"Data", "Art"}) {
                const auto root = folder / top;
                if(!std::filesystem::is_directory(root, ec)) {
                    continue;
                }
                for(auto it = std::filesystem::recursive_directory_iterator(root, ec);
                    !ec && it != std::filesystem::recursive_directory_iterator();
                    it.increment(ec)) {
                    if(!it->is_regular_file(ec)) {
                        continue;
                    }
                    const auto relative = std::filesystem::relative(it->path(), folder, ec).generic_string();
                    this->sources.emplace(normalizeArchivePath(relative), Source{-1, {}, it->path()});
                }
            }

            std::vector<std::filesystem::path> archivePaths;
            for(const auto &item: std::filesystem::directory_iterator(folder, ec)) {
                if(item.is_regular_file(ec) && normalizeArchivePath(item.path().extension().string()) == ".big") {
                    archivePaths.push_back(item.path());
                }
            }
            std::sort(archivePaths.begin(), archivePaths.end(), [](const auto &a, const auto &b) {
                return normalizeArchivePath(a.filename().string()) < normalizeArchivePath(b.filename().string());
            });
            for(const auto &path: archivePaths) {
                std::string archiveError;
                auto archive = BigArchive::open(path, archiveError);
                if(!archive) {
                    warnings.push_back(path.filename().string() + ": " + archiveError);
                    continue;
                }
                const int index = int(this->archives.size());
                for(const auto &entry: archive->entries()) {
                    this->sources.emplace(entry.path, Source{index, entry, {}});
                }
                this->archives.push_back(std::move(*archive));
            }
        }
        return true;
    }

    bool GameFiles::read(const std::string &path, std::string &out) const {
        return this->readPrefix(path, UINT32_MAX, out);
    }

    bool GameFiles::readPrefix(const std::string &path, std::uint32_t maxBytes, std::string &out) const {
        const auto found = this->sources.find(path);
        if(found == this->sources.end()) {
            return false;
        }
        const auto &source = found->second;
        if(source.archive >= 0) {
            return this->archives[std::size_t(source.archive)].readPrefix(source.entry, maxBytes, out);
        }
        std::ifstream file(source.looseFile, std::ios::binary);
        if(!file) {
            return false;
        }
        file.seekg(0, std::ios::end);
        const auto size = std::uint64_t(file.tellg());
        file.seekg(0);
        out.resize(std::size_t(std::min<std::uint64_t>(size, maxBytes)));
        return bool(file.read(out.data(), std::streamsize(out.size())));
    }

    std::vector<std::string> GameFiles::list(const std::function<bool(const std::string &)> &keep) const {
        std::vector<std::string> result;
        for(const auto &item: this->sources) {
            if(keep(item.first)) {
                result.push_back(item.first);
            }
        }
        return result;
    }
}
