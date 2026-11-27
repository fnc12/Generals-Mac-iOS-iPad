#include "terrain_ini.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace end_editor {

    namespace {
        std::string lower(std::string_view text) {
            std::string result(text);
            for (auto& c: result) {
                if (c >= 'A' && c <= 'Z') {
                    c = char(c - 'A' + 'a');
                }
            }
            return result;
        }

        bool equalsNoCase(std::string_view a, std::string_view b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       const auto lowerChar = [](char c) {
                           return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
                       };
                       return lowerChar(x) == lowerChar(y);
                   });
        }

        std::string_view trim(std::string_view s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
                s.remove_prefix(1);
            }
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
                s.remove_suffix(1);
            }
            return s;
        }

        // One "Keyword Name ... End" block with its "Key = Value" lines, viewed in the text.
        struct Block {
            std::string_view keyword;
            std::string_view name;
            std::vector<std::pair<std::string_view, std::string_view>> fields;  // key, value
        };

        // Calls `visit` with every top-level block, one after the other (one Block is reused);
        // these files have no nested blocks.
        template<class F>
        void forEachBlock(std::string_view text, F&& visit) {
            Block block;
            bool inside = false;
            std::size_t start = 0;
            while (start <= text.size()) {
                auto end = text.find('\n', start);
                if (end == std::string_view::npos) {
                    end = text.size();
                }
                auto line = text.substr(start, end - start);
                start = end + 1;
                line = trim(line.substr(0, std::min(line.find(';'), line.find("//"))));
                if (line.empty()) {
                    continue;
                }
                const auto equals = line.find('=');
                if (!inside) {
                    if (equals != std::string_view::npos) {
                        continue;
                    }
                    const auto space = line.find_first_of(" \t");
                    block.keyword = line.substr(0, space);
                    block.name = space == std::string_view::npos ? std::string_view() : trim(line.substr(space));
                    block.fields.clear();
                    inside = true;
                } else if (equals == std::string_view::npos) {
                    if (equalsNoCase(line, "end")) {
                        visit(block);
                        inside = false;
                    }
                } else {
                    block.fields.emplace_back(trim(line.substr(0, equals)), trim(line.substr(equals + 1)));
                }
            }
            if (inside) {  // a block the file does not close
                visit(block);
            }
        }

        // "R:200 G:200 B:200 A:255" -> 0..1 values; missing components keep their defaults.
        template<std::size_t N>
        void readColor(std::string_view view, std::array<float, N>& out) {
            const std::string value(view);  // atof needs the terminating zero
            const char names[] = {'R', 'G', 'B', 'A'};
            for (std::size_t i = 0; i < N; ++i) {
                const char name[] = {names[i], ':', '\0'};
                const auto at = value.find(name);
                if (at != std::string::npos) {
                    out[i] = float(std::atof(value.c_str() + at + 2)) / 255.0f;
                }
            }
        }
    }

    std::map<std::string, std::string> parseTerrainTypes(std::string_view text) {
        std::map<std::string, std::string> result;
        forEachBlock(text, [&](const Block& block) {
            if (!equalsNoCase(block.keyword, "terrain")) {
                return;
            }
            for (const auto& [key, value]: block.fields) {
                if (equalsNoCase(key, "texture")) {
                    result[lower(block.name)] = value;
                }
            }
        });
        return result;
    }

    std::vector<RoadType> parseRoadTypes(std::string_view text) {
        std::vector<RoadType> result;
        forEachBlock(text, [&](const Block& block) {
            const bool bridge = equalsNoCase(block.keyword, "bridge");
            if ((bridge || equalsNoCase(block.keyword, "road")) && !block.name.empty()) {
                result.push_back(RoadType{std::string(block.name), bridge});
            }
        });
        return result;
    }

    std::vector<Faction> parsePlayableFactions(std::string_view text) {
        std::vector<Faction> result;
        forEachBlock(text, [&](const Block& block) {
            if (!equalsNoCase(block.keyword, "playertemplate") || block.name.empty()) {
                return;
            }
            bool playable = false;
            std::string_view side;
            for (const auto& [key, value]: block.fields) {
                if (equalsNoCase(key, "playableside")) {
                    playable = equalsNoCase(value, "yes");
                } else if (equalsNoCase(key, "side")) {
                    side = value;
                }
            }
            if (playable) {
                result.push_back(Faction{std::string(block.name), std::string(side)});
            }
        });
        return result;
    }

    std::vector<PlayerColor> parsePlayerColors(std::string_view text) {
        std::vector<PlayerColor> result;
        forEachBlock(text, [&](const Block& block) {
            if (!equalsNoCase(block.keyword, "multiplayercolor") || block.name.empty()) {
                return;
            }
            PlayerColor color{std::string(block.name)};
            for (const auto& [key, value]: block.fields) {
                if (equalsNoCase(key, "rgbcolor")) {
                    std::array<float, 3> rgb{1, 1, 1};
                    readColor(value, rgb);
                    color.rgb = 0;
                    for (const auto component: rgb) {
                        color.rgb = color.rgb << 8 | std::uint32_t(std::clamp(component * 255.0f + 0.5f, 0.0f, 255.0f));
                    }
                }
            }
            result.push_back(std::move(color));
        });
        return result;
    }

    WaterLook parseWaterLook(std::string_view text, int timeOfDay) {
        static const char* const times[] = {"morning", "afternoon", "evening", "night"};
        const std::string_view wanted = timeOfDay >= 1 && timeOfDay <= 4 ? times[timeOfDay - 1] : "afternoon";
        WaterLook look;
        forEachBlock(text, [&](const Block& block) {
            if (equalsNoCase(block.keyword, "waterset") && equalsNoCase(block.name, wanted)) {
                for (const auto& [key, value]: block.fields) {
                    if (equalsNoCase(key, "watertexture")) {
                        look.texture = value;
                    } else if (equalsNoCase(key, "diffusecolor")) {
                        readColor(value, look.diffuse);
                    } else if (equalsNoCase(key, "transparentdiffusecolor")) {
                        readColor(value, look.transparentDiffuse);
                    } else if (equalsNoCase(key, "waterrepeatcount")) {
                        look.repeatCount = float(std::atof(std::string(value).c_str()));
                    }
                }
            } else if (equalsNoCase(block.keyword, "watertransparency")) {
                for (const auto& [key, value]: block.fields) {
                    if (equalsNoCase(key, "standingwatertexture")) {
                        look.standingTexture = value;
                    } else if (equalsNoCase(key, "standingwatercolor")) {
                        readColor(value, look.standingColor);
                    }
                }
            }
        });
        return look;
    }
}
