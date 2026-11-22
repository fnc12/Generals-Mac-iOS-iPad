#pragma once

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

    // Reads the object templates of a game folder: every Data/INI/Object/*.ini (and
    // Data/INI/Default/Object.ini) from the folder's .big archives and from loose files under
    // the folder. A loose file wins over an archived one with the same path, and among archives
    // the first in name order wins (that is why patch archives are named "!..." or "00...").
    GameDataScan scanGameData(const std::filesystem::path &gameDir);

    // True for the INI paths that hold object templates ("data/ini/object/x.ini").
    bool isObjectIniPath(const std::string &normalizedPath);
}
