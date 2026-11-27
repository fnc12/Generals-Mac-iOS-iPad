#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace end_editor {

    // Data/INI/Terrain.ini: texture class name (lower case) -> TGA file under Art/Terrain/.
    std::map<std::string, std::string> parseTerrainTypes(std::string_view text);

    // Data/INI/Roads.ini: the road and bridge types a road or bridge segment is made of (the
    // template name of its two map objects), in file order.
    struct RoadType {
        std::string name;
        bool bridge = false;
    };
    std::vector<RoadType> parseRoadTypes(std::string_view text);

    // Data/INI/PlayerTemplate.ini: the factions a player can play (PlayableSide = Yes), in file
    // order: what the game's -testMapFaction takes.
    struct Faction {
        std::string name;  // e.g. "FactionAmerica"
        std::string side;  // e.g. "America"
    };
    std::vector<Faction> parsePlayableFactions(std::string_view text);

    // What Data/INI/Water.ini says about standing water for one time of day.
    struct WaterLook {
        std::string texture;                        // WaterSet's WaterTexture
        std::array<float, 4> diffuse{1, 1, 1, 1};   // DiffuseColor, 0..1
        std::array<float, 4> transparentDiffuse{1, 1, 1, 0.5f};  // TransparentDiffuseColor (alpha = opacity)
        float repeatCount = 32;                     // WaterRepeatCount
        std::string standingTexture;                // WaterTransparency's StandingWaterTexture
        std::array<float, 3> standingColor{1, 1, 1};  // StandingWaterColor
    };

    // `timeOfDay`: 1 morning, 2 afternoon, 3 evening, 4 night (GlobalLighting's numbering).
    WaterLook parseWaterLook(std::string_view text, int timeOfDay);
}
