#include "map_preview.h"

#include <algorithm>
#include <cmath>

namespace end_editor {

    namespace {
        using Color = std::array<float, 3>;

        // MapPreview::interpolateColorForHeight: lighter above the middle height, darker below.
        Color forHeight(Color color, float height, float high, float middle, float low) {
            constexpr float bright = 0.30f, dark = 0.60f;
            if(high == middle) {
                high = middle + 0.1f;
            }
            if(middle == low) {
                low = middle - 0.1f;
            }
            if(high == low) {
                high = low + 0.2f;
            }
            Color target;
            float t;
            if(height >= middle) {
                t = (height - middle) / (high - middle);
                for(int i = 0; i < 3; ++i) {
                    target[std::size_t(i)] = color[std::size_t(i)] + (1 - color[std::size_t(i)]) * bright;
                }
            } else {
                t = (middle - height) / (middle - low);
                for(int i = 0; i < 3; ++i) {
                    target[std::size_t(i)] = color[std::size_t(i)] - color[std::size_t(i)] * dark;
                }
            }
            for(int i = 0; i < 3; ++i) {
                auto &c = color[std::size_t(i)];
                c = std::clamp(c + (target[std::size_t(i)] - c) * t, 0.0f, 1.0f);
            }
            return color;
        }

        bool inside(const MapPolygon &polygon, float x, float y) {
            bool in = false;
            const auto &p = polygon.points;
            for(std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
                const float xi = float(p[i][0]), yi = float(p[i][1]), xj = float(p[j][0]), yj = float(p[j][1]);
                if((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
                    in = !in;
                }
            }
            return in;
        }
    }

    std::vector<std::uint8_t> renderMapPreview(const MapFile &map, const std::vector<std::array<float, 3>> &tileColors) {
        constexpr int size = mapPreviewSize;
        std::vector<std::uint8_t> rgba(std::size_t(size) * size * 4, 255);
        if(map.heights.empty()) {
            return rgba;
        }
        const float border = float(map.border);
        const float xSample = float(map.width - 2 * map.border) / size;
        const float ySample = float(map.height - 2 * map.border) / size;
        // A preview pixel's point in cells (MapPreview::mapPreviewToWorld) and what is there.
        const auto cell = [&](int x, int y) {
            return std::array<float, 2>{float(std::clamp(x, 0, size - 1)) * xSample + border,
                                        float(std::clamp(y, 0, size - 1)) * ySample + border};
        };
        const auto heightAt = [&](std::array<float, 2> at) {  // the height byte, like getHeight(Int, Int)
            const int i = std::clamp(int(at[0]), 0, map.width - 1), j = std::clamp(int(at[1]), 0, map.height - 1);
            return float(map.heights[std::size_t(j * map.width + i)]);
        };
        const auto colorAt = [&](std::array<float, 2> at) {  // getTerrainColorAt
            const int i = std::clamp(int(std::floor(at[0])), 0, map.width - 1);
            const int j = std::clamp(int(std::floor(at[1])), 0, map.height - 1);
            const auto index = std::size_t(j * map.width + i);
            const int tile = index < map.tiles.size() ? map.tiles[index] >> 2 : -1;
            return tile >= 0 && std::size_t(tile) < tileColors.size() ? tileColors[std::size_t(tile)] : Color{0, 0, 0};
        };
        const auto underwater = [&](std::array<float, 2> at) {  // localIsUnderwater, in world units
            const float x = std::floor(mapXYFactor * (at[0] - border) + 0.5f);
            const float y = std::floor(mapXYFactor * (at[1] - border) + 0.5f);
            for(const auto &polygon: map.polygons) {
                if(polygon.water && polygon.points.size() >= 3 && inside(polygon, x, y)) {
                    return groundHeight(map, x, y) < float(polygon.points.front()[2]);
                }
            }
            return false;
        };

        float sum = 0, high = 0, low = 100;  // MapPreview starts its minimum at 100
        for(int y = 0; y < size; ++y) {
            for(int x = 0; x < size; ++x) {
                const auto h = heightAt(cell(x, y));
                sum += h;
                high = std::max(high, h);
                low = std::min(low, h);
            }
        }
        const float average = sum / float(size * size);
        const auto [mapLow, mapHigh] = std::minmax_element(map.heights.begin(), map.heights.end());
        const Color water{0.55f, 0.55f, 1.0f};

        for(int y = 0; y < size; ++y) {
            for(int x = 0; x < size; ++x) {
                const auto center = cell(x, y);
                const bool wet = underwater(center);
                const float z = heightAt(center);
                Color total{0, 0, 0};
                int samples = 0;
                for(int j = y - 1; j <= y + 1; ++j) {
                    for(int i = x - 1; i <= x + 1; ++i) {
                        if(i < 0 || j < 0 || i >= size || j >= size) {
                            continue;
                        }
                        const auto at = cell(i, j);
                        Color color;
                        if(wet) {
                            if(!underwater(at)) {
                                continue;
                            }
                            color = forHeight(water, heightAt(at), float(*mapHigh), average, float(*mapLow));
                        } else {
                            color = forHeight(colorAt(at), z, high, average, low);
                        }
                        for(int c = 0; c < 3; ++c) {
                            total[std::size_t(c)] += color[std::size_t(c)];
                        }
                        ++samples;
                    }
                }
                auto *pixel = &rgba[(std::size_t(y) * size + std::size_t(x)) * 4];
                for(int c = 0; c < 3; ++c) {
                    pixel[c] = std::uint8_t(std::lround(total[std::size_t(c)] / float(std::max(samples, 1)) * 255));
                }
                pixel[3] = 255;
            }
        }
        return rgba;
    }

    std::string encodeTga(int width, int height, const std::vector<std::uint8_t> &rgba) {
        std::string out(18, '\0');
        out[2] = 2;  // uncompressed true color
        out[12] = char(width & 0xFF);
        out[13] = char(width >> 8);
        out[14] = char(height & 0xFF);
        out[15] = char(height >> 8);
        out[16] = 32;
        out[17] = 0;  // origin bottom left, like the shipped previews
        out.reserve(out.size() + rgba.size() + 26);
        for(std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
            out.push_back(char(rgba[i + 2]));
            out.push_back(char(rgba[i + 1]));
            out.push_back(char(rgba[i]));
            out.push_back(char(rgba[i + 3]));
        }
        out.append(8, '\0');  // no extension or developer area
        out.append("TRUEVISION-XFILE.");
        out.push_back('\0');
        return out;
    }
}
