#pragma once

#include "game_files.h"
#include "ini_objects.h"

#include <filesystem>
#include <string>
#include <vector>

namespace end_editor {

    struct GameDataScan {
        std::vector<ObjectInfo> objects;
        std::vector<std::string> warnings;
        int archiveCount = 0;
        int iniFileCount = 0;
        std::string error;  // set when nothing could be read
    };

    // Reads the object templates (every Data/INI/Object/*.ini and Data/INI/Default/Object.ini)
    // the mounted folders hold. See GameFiles for which copy of a file wins.
    GameDataScan scanObjects(const GameFiles& files);

    // Mounts `folders` (Zero Hour first, then base Generals if given) and scans them.
    // `files` keeps the mount for later reads (models, textures).
    GameDataScan scanGameData(const std::vector<std::filesystem::path>& folders, GameFiles& files);
    GameDataScan scanGameData(const std::filesystem::path& gameDir);

    // True for the INI paths that hold object templates ("data/ini/object/x.ini").
    bool isObjectIniPath(const std::string& normalizedPath);
}
