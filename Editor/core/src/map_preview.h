#pragma once

#include "map_file.h"

#include <array>
#include <string>
#include <vector>

namespace end_editor {

    // The picture the game's map lists show: WorldBuilder writes it next to the map as
    // <name>.tga when it saves (MapPreview.cpp), and MapUtil's getMapPreviewImage looks for it
    // there. 128 x 128 pixels of the playable area: each tile's average color made lighter or
    // darker by its height, water areas light blue, every pixel the average of its 3 x 3
    // neighbours like WorldBuilder's.
    constexpr int mapPreviewSize = 128;

    // RGBA8, rows from south (y = 0) to north. `tileColors`: MapScene::tileColors.
    std::vector<std::uint8_t> renderMapPreview(const MapFile& map, const std::vector<std::array<float, 3>>& tileColors);

    // An uncompressed 32-bit TGA (type 2, origin bottom left, TGA 2 footer), the layout of the
    // previews the game ships. `rgba` rows from the bottom.
    std::string encodeTga(int width, int height, const std::vector<std::uint8_t>& rgba);
}
