#include "texture.h"

#include <cstring>

namespace end_editor {

    namespace {
        std::uint32_t le32(std::string_view data, std::size_t offset) {
            std::uint32_t value = 0;
            std::memcpy(&value, data.data() + offset, 4);
            return value;
        }

        std::uint16_t le16(std::string_view data, std::size_t offset) {
            return std::uint16_t(std::uint8_t(data[offset]) | (std::uint8_t(data[offset + 1]) << 8));
        }

        constexpr std::uint32_t maxSide = 8192;

        // Expands an RGB565 color to 8 bits per channel.
        void color565(std::uint16_t c, std::uint8_t* rgb) {
            const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            rgb[0] = std::uint8_t((r << 3) | (r >> 2));
            rgb[1] = std::uint8_t((g << 2) | (g >> 4));
            rgb[2] = std::uint8_t((b << 3) | (b >> 2));
        }

        // The color part of a DXT block (8 bytes) into 16 RGBA pixels. DXT1 blocks with
        // c0 <= c1 have three colors and transparent black.
        void decodeColorBlock(const std::uint8_t* block, bool dxt1, std::uint8_t pixels[16][4]) {
            const std::uint16_t c0 = std::uint16_t(block[0] | (block[1] << 8));
            const std::uint16_t c1 = std::uint16_t(block[2] | (block[3] << 8));
            std::uint8_t colors[4][4];
            color565(c0, colors[0]);
            color565(c1, colors[1]);
            colors[0][3] = colors[1][3] = 255;
            if (!dxt1 || c0 > c1) {
                for (int k = 0; k < 3; ++k) {
                    colors[2][k] = std::uint8_t((2 * colors[0][k] + colors[1][k]) / 3);
                    colors[3][k] = std::uint8_t((colors[0][k] + 2 * colors[1][k]) / 3);
                }
                colors[2][3] = colors[3][3] = 255;
            } else {
                for (int k = 0; k < 3; ++k) {
                    colors[2][k] = std::uint8_t((colors[0][k] + colors[1][k]) / 2);
                    colors[3][k] = 0;
                }
                colors[2][3] = 255;
                colors[3][3] = 0;
            }
            const std::uint32_t bits = std::uint32_t(block[4]) | (std::uint32_t(block[5]) << 8) |
                                       (std::uint32_t(block[6]) << 16) | (std::uint32_t(block[7]) << 24);
            for (int i = 0; i < 16; ++i) {
                std::memcpy(pixels[i], colors[(bits >> (2 * i)) & 3], 4);
            }
        }

        void decodeDxt3Alpha(const std::uint8_t* block, std::uint8_t pixels[16][4]) {
            for (int i = 0; i < 16; ++i) {
                const int nibble = (block[i / 2] >> ((i % 2) * 4)) & 15;
                pixels[i][3] = std::uint8_t(nibble * 17);
            }
        }

        void decodeDxt5Alpha(const std::uint8_t* block, std::uint8_t pixels[16][4]) {
            const int a0 = block[0], a1 = block[1];
            int alphas[8] = {a0, a1};
            if (a0 > a1) {
                for (int k = 1; k <= 6; ++k) {
                    alphas[k + 1] = ((7 - k) * a0 + k * a1) / 7;
                }
            } else {
                for (int k = 1; k <= 4; ++k) {
                    alphas[k + 1] = ((5 - k) * a0 + k * a1) / 5;
                }
                alphas[6] = 0;
                alphas[7] = 255;
            }
            std::uint64_t bits = 0;
            for (int k = 0; k < 6; ++k) {
                bits |= std::uint64_t(block[2 + k]) << (8 * k);
            }
            for (int i = 0; i < 16; ++i) {
                pixels[i][3] = std::uint8_t(alphas[(bits >> (3 * i)) & 7]);
            }
        }
    }

    bool decodeDds(std::string_view bytes, Image& out, std::string& error) {
        if (bytes.size() < 128 || bytes.substr(0, 4) != "DDS ") {
            error = "not a DDS file";
            return false;
        }
        const std::uint32_t height = le32(bytes, 12);
        const std::uint32_t width = le32(bytes, 16);
        const std::uint32_t formatFlags = le32(bytes, 80);
        const std::string_view fourCc = bytes.substr(84, 4);
        if (width == 0 || height == 0 || width > maxSide || height > maxSide) {
            error = "bad DDS size";
            return false;
        }
        if (!(formatFlags & 4)) {
            error = "uncompressed DDS is not supported";
            return false;
        }
        const bool dxt1 = fourCc == "DXT1";
        const bool dxt3 = fourCc == "DXT3";
        const bool dxt5 = fourCc == "DXT5";
        if (!dxt1 && !dxt3 && !dxt5) {
            error = "DDS format " + std::string(fourCc) + " is not supported";
            return false;
        }
        const std::uint32_t blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
        const std::size_t blockSize = dxt1 ? 8 : 16;
        if (128 + std::uint64_t(blocksX) * blocksY * blockSize > bytes.size()) {
            error = "DDS data is short";
            return false;
        }

        out.width = width;
        out.height = height;
        out.rgba.assign(std::size_t(width) * height * 4, 0);
        const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data()) + 128;
        for (std::uint32_t by = 0; by < blocksY; ++by) {
            for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
                const std::uint8_t* block = data + (std::size_t(by) * blocksX + bx) * blockSize;
                std::uint8_t pixels[16][4];
                decodeColorBlock(dxt1 ? block : block + 8, dxt1, pixels);
                if (dxt3) {
                    decodeDxt3Alpha(block, pixels);
                } else if (dxt5) {
                    decodeDxt5Alpha(block, pixels);
                }
                for (std::uint32_t i = 0; i < 16; ++i) {
                    const std::uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    if (x < width && y < height) {
                        std::memcpy(&out.rgba[(std::size_t(y) * width + x) * 4], pixels[i], 4);
                    }
                }
            }
        }
        return true;
    }

    bool decodeTga(std::string_view bytes, Image& out, std::string& error) {
        if (bytes.size() < 18) {
            error = "not a TGA file";
            return false;
        }
        const std::uint8_t idLength = std::uint8_t(bytes[0]);
        const std::uint8_t colorMapType = std::uint8_t(bytes[1]);
        const std::uint8_t imageType = std::uint8_t(bytes[2]);
        const std::uint32_t width = le16(bytes, 12);
        const std::uint32_t height = le16(bytes, 14);
        const std::uint8_t bitsPerPixel = std::uint8_t(bytes[16]);
        const bool topDown = (std::uint8_t(bytes[17]) & 0x20) != 0;
        if (colorMapType != 0 || (imageType != 2 && imageType != 10) || (bitsPerPixel != 24 && bitsPerPixel != 32)) {
            error = "TGA kind is not supported";
            return false;
        }
        if (width == 0 || height == 0) {
            error = "bad TGA size";
            return false;
        }
        const std::size_t pixelSize = bitsPerPixel / 8;
        const std::size_t count = std::size_t(width) * height;
        std::size_t position = 18 + idLength;

        out.width = width;
        out.height = height;
        out.rgba.assign(count * 4, 255);
        const auto put = [&](std::size_t index, const char* bgra) {
            const std::size_t row = index / width, column = index % width;
            const std::size_t y = topDown ? row : height - 1 - row;
            auto* pixel = &out.rgba[(y * width + column) * 4];
            pixel[0] = std::uint8_t(bgra[2]);
            pixel[1] = std::uint8_t(bgra[1]);
            pixel[2] = std::uint8_t(bgra[0]);
            pixel[3] = pixelSize == 4 ? std::uint8_t(bgra[3]) : 255;
        };

        std::size_t index = 0;
        while (index < count) {
            if (imageType == 2) {
                if (position + pixelSize > bytes.size()) {
                    break;
                }
                put(index++, bytes.data() + position);
                position += pixelSize;
                continue;
            }
            if (position >= bytes.size()) {
                break;
            }
            const std::uint8_t header = std::uint8_t(bytes[position++]);
            const std::size_t run = std::size_t(header & 0x7F) + 1;
            if (header & 0x80) {
                if (position + pixelSize > bytes.size()) {
                    break;
                }
                for (std::size_t k = 0; k < run && index < count; ++k) {
                    put(index++, bytes.data() + position);
                }
                position += pixelSize;
            } else {
                for (std::size_t k = 0; k < run && index < count; ++k) {
                    if (position + pixelSize > bytes.size()) {
                        break;
                    }
                    put(index++, bytes.data() + position);
                    position += pixelSize;
                }
            }
        }
        if (index < count) {
            error = "TGA data is short";
            return false;
        }
        return true;
    }
}
