#include "ini_objects.h"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace end_editor {

    namespace {
        bool equalsNoCase(std::string_view a, std::string_view b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       auto lower = [](char c) {
                           return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
                       };
                       return lower(x) == lower(y);
                   });
        }

        std::string_view trim(std::string_view s) {
            const auto isSpace = [](char c) {
                return c == ' ' || c == '\t' || c == '\r' || c == '\n';
            };
            while(!s.empty() && isSpace(s.front())) {
                s.remove_prefix(1);
            }
            while(!s.empty() && isSpace(s.back())) {
                s.remove_suffix(1);
            }
            return s;
        }

        std::string_view stripComment(std::string_view line) {
            const auto semicolon = line.find(';');
            const auto slashes = line.find("//");
            return line.substr(0, std::min(semicolon, slashes));
        }

        // Keywords inside an Object that open a block closed by "End" (taken from the engine's
        // field parse tables: ThingTemplate.cpp, W3DModelDraw.cpp, AIUpdate.cpp, and every field
        // parsed by RadiusDecalTemplate::parseRadiusDecalTemplate).
        constexpr std::array blockKeywords = {
            std::string_view("Draw"),
            std::string_view("Body"),
            std::string_view("Behavior"),
            std::string_view("ClientUpdate"),
            std::string_view("ClientBehavior"),
            std::string_view("ArmorSet"),
            std::string_view("WeaponSet"),
            std::string_view("UnitSpecificSounds"),
            std::string_view("UnitSpecificFX"),
            std::string_view("Prerequisites"),
            std::string_view("ReplaceModule"),
            std::string_view("AddModule"),
            std::string_view("InheritableModule"),
            std::string_view("OverrideableByLikeKind"),
            std::string_view("ConditionState"),
            std::string_view("DefaultConditionState"),
            std::string_view("TransitionState"),
            std::string_view("AttackAreaDecal"),
            std::string_view("TargetingReticleDecal"),
            std::string_view("GridDecalTemplate"),
            std::string_view("DeliveryDecal"),
        };

        bool opensBlock(std::string_view keyword) {
            return std::any_of(blockKeywords.begin(), blockKeywords.end(), [keyword](std::string_view k) {
                return equalsNoCase(k, keyword);
            });
        }

        // AIUpdate's "Turret" block is written without '='; inside a model condition state
        // "Turret = <bone>" is a plain field.
        bool opensBlockWithoutValue(std::string_view keyword) {
            return equalsNoCase(keyword, "Turret") || equalsNoCase(keyword, "AltTurret");
        }

        bool isObjectDefinition(std::string_view keyword) {
            return equalsNoCase(keyword, "Object") || equalsNoCase(keyword, "ObjectReskin") ||
                   equalsNoCase(keyword, "ChildObject");
        }

        // Splits "Key = value" or "Key value words" into key and the rest.
        void splitLine(std::string_view line, std::string_view &key, std::string_view &rest) {
            std::size_t end = 0;
            while(end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '=') {
                ++end;
            }
            key = line.substr(0, end);
            rest = trim(line.substr(end));
            if(!rest.empty() && rest.front() == '=') {
                rest = trim(rest.substr(1));
            }
        }

        std::string_view firstWord(std::string_view s) {
            const auto end = s.find_first_of(" \t");
            return s.substr(0, end);
        }
    }

    ObjectParseResult parseObjects(std::string_view text, std::string_view sourceFile) {
        ObjectParseResult result;
        ObjectInfo current;
        std::vector<std::string_view> blocks;  // open blocks; blocks[0] is the object itself
        bool modelFromDefaultState = false;
        int lineNumber = 0;

        auto warn = [&](const std::string &message) {
            result.warnings.push_back(std::string(sourceFile) + ":" + std::to_string(lineNumber) + ": " + message);
        };

        std::size_t position = 0;
        while(position <= text.size()) {
            auto lineEnd = text.find('\n', position);
            if(lineEnd == std::string_view::npos) {
                lineEnd = text.size();
            }
            const auto line = trim(stripComment(text.substr(position, lineEnd - position)));
            position = lineEnd + 1;
            ++lineNumber;
            if(line.empty()) {
                continue;
            }

            std::string_view key, rest;
            splitLine(line, key, rest);

            if(blocks.empty()) {
                if(!isObjectDefinition(key)) {
                    warn("unexpected line outside an object: " + std::string(key));
                    continue;
                }
                current = ObjectInfo{};
                current.definition = std::string(key);
                current.name = std::string(firstWord(rest));
                const auto afterName = trim(rest.substr(std::min(rest.size(), current.name.size())));
                current.parent = std::string(firstWord(afterName));
                current.sourceFile = std::string(sourceFile);
                current.line = lineNumber;
                modelFromDefaultState = false;
                blocks.push_back(key);
                continue;
            }

            if(equalsNoCase(key, "End")) {
                blocks.pop_back();
                if(blocks.empty()) {
                    result.objects.push_back(std::move(current));
                }
                continue;
            }
            if(opensBlock(key) || (rest.empty() && opensBlockWithoutValue(key))) {
                blocks.push_back(key);
                continue;
            }

            if(blocks.size() == 1) {
                const std::string value(rest);
                if(equalsNoCase(key, "Side")) {
                    current.side = value;
                } else if(equalsNoCase(key, "KindOf")) {
                    current.kindOf = value;
                } else if(equalsNoCase(key, "Buildable")) {
                    current.buildable = value;
                } else if(equalsNoCase(key, "EditorSorting")) {
                    current.editorSorting = value;
                } else if(equalsNoCase(key, "DisplayName")) {
                    current.displayName = value;
                }
            } else if(equalsNoCase(key, "Model") && equalsNoCase(blocks[1], "Draw")) {
                const bool inDefaultState = equalsNoCase(blocks.back(), "DefaultConditionState");
                if(current.model.empty() || (inDefaultState && !modelFromDefaultState)) {
                    current.model = std::string(firstWord(rest));
                    modelFromDefaultState = inDefaultState;
                }
            }
        }

        if(!blocks.empty()) {
            ++lineNumber;
            warn("file ends inside object " + current.name);
        }
        return result;
    }

    void resolveInheritance(std::vector<ObjectInfo> &objects) {
        std::unordered_map<std::string, std::size_t> byName;
        for(std::size_t i = 0; i < objects.size(); ++i) {
            byName.emplace(objects[i].name, i);
        }
        // Parents can be children themselves; resolve each chain from its root. Depth is capped so
        // a cycle in broken data cannot loop forever.
        for(auto &object: objects) {
            const ObjectInfo *parent = &object;
            for(int depth = 0; depth < 16 && !parent->parent.empty(); ++depth) {
                const auto found = byName.find(parent->parent);
                if(found == byName.end()) {
                    break;
                }
                parent = &objects[found->second];
                auto inherit = [](std::string &field, const std::string &from) {
                    if(field.empty()) {
                        field = from;
                    }
                };
                inherit(object.side, parent->side);
                inherit(object.kindOf, parent->kindOf);
                inherit(object.buildable, parent->buildable);
                inherit(object.editorSorting, parent->editorSorting);
                inherit(object.displayName, parent->displayName);
                inherit(object.model, parent->model);
            }
        }
    }
}
