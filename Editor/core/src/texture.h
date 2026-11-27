#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace end_editor {

    // 8-bit RGBA pixels, rows from top to bottom.
    struct Image {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba;
    };

    // The texture formats the game ships: DDS with DXT1/DXT3/DXT5 (the first mip level is
    // decoded) and uncompressed or RLE TGA, 24 or 32 bit. Returns false and sets `error`
    // for anything else.
    bool decodeDds(std::string_view bytes, Image& out, std::string& error);
    bool decodeTga(std::string_view bytes, Image& out, std::string& error);
}
