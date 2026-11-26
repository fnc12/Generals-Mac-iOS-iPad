#include "terrain_ini.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace end_editor {

    namespace {
        std::string lower(std::string_view text) {
            std::string result(text);
            for(auto &c: result) {
                if(c >= 'A' && c <= 'Z') {
                    c = char(c - 'A' + 'a');
                }
            }
            return result;
        }

        std::string_view trim(std::string_view s) {
            while(!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
                s.remove_prefix(1);
            }
            while(!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
                s.remove_suffix(1);
            }
            return s;
        }

        // One "Keyword Name ... End" block with its "Key = Value" lines.
        struct Block {
            std::string keyword;  // lower case
            std::string name;
            std::vector<std::pair<std::string, std::string>> fields;  // lower-case key, value
        };

        // Top-level blocks only; these files have no nested blocks.
        std::vector<Block> readBlocks(std::string_view text) {
            std::vector<Block> blocks;
            bool inside = false;
            std::size_t start = 0;
            while(start <= text.size()) {
                auto end = text.find('\n', start);
                if(end == std::string_view::npos) {
                    end = text.size();
                }
                auto line = text.substr(start, end - start);
                start = end + 1;
                line = trim(line.substr(0, std::min(line.find(';'), line.find("//"))));
                if(line.empty()) {
                    continue;
                }
                const auto equals = line.find('=');
                if(!inside) {
                    if(equals != std::string_view::npos) {
                        continue;
                    }
                    const auto space = line.find_first_of(" \t");
                    Block block;
                    block.keyword = lower(line.substr(0, space));
                    block.name = space == std::string_view::npos ? std::string() : std::string(trim(line.substr(space)));
                    blocks.push_back(std::move(block));
                    inside = true;
                } else if(equals == std::string_view::npos) {
                    if(lower(line) == "end") {
                        inside = false;
                    }
                } else {
                    blocks.back().fields.emplace_back(lower(trim(line.substr(0, equals))),
                                                      std::string(trim(line.substr(equals + 1))));
                }
            }
            return blocks;
        }

        // "R:200 G:200 B:200 A:255" -> 0..1 values; missing components keep their defaults.
        template<std::size_t N>
        void readColor(const std::string &value, std::array<float, N> &out) {
            const char names[] = {'R', 'G', 'B', 'A'};
            for(std::size_t i = 0; i < N; ++i) {
                const auto at = value.find(std::string(1, names[i]) + ":");
                if(at != std::string::npos) {
                    out[i] = float(std::atof(value.c_str() + at + 2)) / 255.0f;
                }
            }
        }
    }

    std::map<std::string, std::string> parseTerrainTypes(std::string_view text) {
        std::map<std::string, std::string> result;
        for(const auto &block: readBlocks(text)) {
            if(block.keyword != "terrain") {
                continue;
            }
            for(const auto &[key, value]: block.fields) {
                if(key == "texture") {
                    result[lower(block.name)] = value;
                }
            }
        }
        return result;
    }

    std::vector<RoadType> parseRoadTypes(std::string_view text) {
        std::vector<RoadType> result;
        for(const auto &block: readBlocks(text)) {
            if((block.keyword == "road" || block.keyword == "bridge") && !block.name.empty()) {
                result.push_back(RoadType{block.name, block.keyword == "bridge"});
            }
        }
        return result;
    }

    WaterLook parseWaterLook(std::string_view text, int timeOfDay) {
        static const char *const times[] = {"morning", "afternoon", "evening", "night"};
        const std::string wanted = timeOfDay >= 1 && timeOfDay <= 4 ? times[timeOfDay - 1] : "afternoon";
        WaterLook look;
        for(const auto &block: readBlocks(text)) {
            if(block.keyword == "waterset" && lower(block.name) == wanted) {
                for(const auto &[key, value]: block.fields) {
                    if(key == "watertexture") {
                        look.texture = value;
                    } else if(key == "diffusecolor") {
                        readColor(value, look.diffuse);
                    } else if(key == "transparentdiffusecolor") {
                        readColor(value, look.transparentDiffuse);
                    } else if(key == "waterrepeatcount") {
                        look.repeatCount = float(std::atof(value.c_str()));
                    }
                }
            } else if(block.keyword == "watertransparency") {
                for(const auto &[key, value]: block.fields) {
                    if(key == "standingwatertexture") {
                        look.standingTexture = value;
                    } else if(key == "standingwatercolor") {
                        readColor(value, look.standingColor);
                    }
                }
            }
        }
        return look;
    }
}
