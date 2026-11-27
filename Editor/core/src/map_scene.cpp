#include "map_scene.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace end_editor {

    namespace {
        constexpr int tilePixels = 64;  // a source tile; a cell shows a 32 x 32 quarter of it
        constexpr int atlasPadding = 4;  // wrapped border around each class, like TerrainTex.cpp
        constexpr int atlasMinWidth = 2048;
        // Cliff u/v were saved for WorldBuilder's 2048-pixel atlas of 64-pixel tiles.
        constexpr float cliffAtlasPixels = 2048;

        std::string lower(std::string text) {
            for (auto& c: text) {
                if (c >= 'A' && c <= 'Z') {
                    c = char(c - 'A' + 'a');
                }
            }
            return text;
        }

        // Nearest-neighbour resize to a square of `size` pixels (an image of that size is moved
        // through).
        Image resizeSquare(Image image, std::uint32_t size) {
            if (image.width == size && image.height == size) {
                return image;
            }
            Image result;
            result.width = result.height = size;
            result.rgba.resize(std::size_t(size) * size * 4);
            for (std::uint32_t y = 0; y < size; ++y) {
                const auto sy = std::min(image.height - 1, y * image.height / size);
                for (std::uint32_t x = 0; x < size; ++x) {
                    const auto sx = std::min(image.width - 1, x * image.width / size);
                    std::copy_n(&image.rgba[(std::size_t(sy) * image.width + sx) * 4],
                                4,
                                &result.rgba[(std::size_t(y) * size + x) * 4]);
                }
            }
            return result;
        }

        // Where each texture class sits in the atlas, in pixels (top-left of its tiles).
        struct AtlasSlot {
            int x = 0, y = 0, size = 0;
        };

        class TerrainBuilder {
          public:
            TerrainBuilder(const MapFile& map, const GameFiles& files, MapScene& scene) :
                map(map), files(files), scene(scene) {}

            void buildAtlas() {
                // The first Terrain.ini wins; older copies (base Generals under Zero Hour) fill
                // in the names it lacks, which some shipped maps still use.
                std::map<std::string, std::string> types;
                const auto copies = this->files.readEach("data/ini/terrain.ini", [&](const std::string& text) {
                    types.merge(parseTerrainTypes(text));
                });
                if (copies == 0) {
                    this->scene.warnings.push_back("Data/INI/Terrain.ini not found");
                }
                std::vector<Image> images;
                images.reserve(this->map.textureClasses.size() + 1);
                for (const auto& textureClass: this->map.textureClasses) {
                    const int size = std::clamp(textureClass.width, 1, 16) * tilePixels;
                    Image image;
                    const auto found = types.find(lower(textureClass.name));
                    std::string error;
                    if (found == types.end()) {
                        this->scene.warnings.push_back("terrain " + textureClass.name + " is not in Terrain.ini");
                    } else if (!readGameTexture(this->files, "art/terrain/", found->second, image, error)) {
                        this->scene.warnings.push_back("terrain " + textureClass.name + ": " + error);
                    }
                    if (image.width == 0) {
                        image = this->placeholder(std::uint32_t(size));
                    }
                    images.push_back(resizeSquare(std::move(image), std::uint32_t(size)));
                }
                images.push_back(this->placeholder(tilePixels));  // for tiles of no class

                // Shelf packing, largest first.
                std::vector<std::size_t> order(images.size());
                for (std::size_t i = 0; i < order.size(); ++i) {
                    order[i] = i;
                }
                std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
                    return images[a].width > images[b].width;
                });
                int width = atlasMinWidth;
                for (const auto& image: images) {
                    width = std::max(width, int(image.width) + 2 * atlasPadding);
                }
                this->slots.assign(images.size(), AtlasSlot{});
                int x = 0, y = 0, shelf = 0;
                for (const auto index: order) {
                    const int slot = int(images[index].width) + 2 * atlasPadding;
                    if (x + slot > width) {
                        x = 0;
                        y += shelf;
                        shelf = 0;
                    }
                    this->slots[index] = AtlasSlot{x + atlasPadding, y + atlasPadding, int(images[index].width)};
                    x += slot;
                    shelf = std::max(shelf, slot);
                }
                const int height = (y + shelf + 3) / 4 * 4;

                auto& atlas = this->scene.atlas;
                atlas.width = std::uint32_t(width);
                atlas.height = std::uint32_t(std::max(height, 4));
                atlas.rgba.assign(std::size_t(atlas.width) * atlas.height * 4, 0);
                for (std::size_t i = 0; i < images.size(); ++i) {
                    const auto& slot = this->slots[i];
                    const auto& image = images[i];
                    for (int dy = -atlasPadding; dy < slot.size + atlasPadding; ++dy) {
                        const int sy = (dy + slot.size) % slot.size;
                        for (int dx = -atlasPadding; dx < slot.size + atlasPadding; ++dx) {
                            const int sx = (dx + slot.size) % slot.size;
                            std::copy_n(
                                &image.rgba[(std::size_t(sy) * image.width + std::size_t(sx)) * 4],
                                4,
                                &atlas.rgba[(std::size_t(slot.y + dy) * atlas.width + std::size_t(slot.x + dx)) * 4]);
                        }
                    }
                }

                // Tile number -> texture class, and each tile's average color (its 64 px square
                // of the class image, rows from the image's bottom).
                for (std::size_t c = 0; c < this->map.textureClasses.size(); ++c) {
                    const auto& textureClass = this->map.textureClasses[c];
                    const auto& image = images[c];
                    const int width = std::clamp(textureClass.width, 1, 16);
                    for (int t = 0; t < textureClass.numTiles && textureClass.firstTile >= 0; ++t) {
                        const auto tile = std::size_t(textureClass.firstTile + t);
                        if (tile >= this->tileClass.size()) {
                            this->tileClass.resize(tile + 1, -1);
                            this->scene.tileColors.resize(tile + 1, {0, 0, 0});
                        }
                        this->tileClass[tile] = int(c);
                        const int column = t % width, row = (t / width) % width;
                        const int x0 = column * tilePixels, y0 = int(image.height) - (row + 1) * tilePixels;
                        double sum[3] = {0, 0, 0};
                        int count = 0;
                        for (int y = std::max(y0, 0); y < std::min(y0 + tilePixels, int(image.height)); ++y) {
                            for (int x = x0; x < std::min(x0 + tilePixels, int(image.width)); ++x) {
                                const auto* pixel = &image.rgba[(std::size_t(y) * image.width + std::size_t(x)) * 4];
                                sum[0] += pixel[0];
                                sum[1] += pixel[1];
                                sum[2] += pixel[2];
                                ++count;
                            }
                        }
                        if (count > 0) {
                            this->scene.tileColors[tile] = {float(sum[0] / count / 255),
                                                            float(sum[1] / count / 255),
                                                            float(sum[2] / count / 255)};
                        }
                    }
                }
            }

            void buildChunks() {
                const int cellsX = this->map.width - 1, cellsY = this->map.height - 1;
                for (int y0 = 0; y0 < cellsY; y0 += terrainChunkCells) {
                    for (int x0 = 0; x0 < cellsX; x0 += terrainChunkCells) {
                        TerrainChunk chunk;
                        chunk.x = x0;
                        chunk.y = y0;
                        const auto cells = std::size_t(std::min(terrainChunkCells, cellsX - x0)) *
                                           std::size_t(std::min(terrainChunkCells, cellsY - y0));
                        chunk.vertices.reserve(cells * 4);
                        chunk.indices.reserve(cells * 6);
                        for (int y = y0; y < std::min(y0 + terrainChunkCells, cellsY); ++y) {
                            for (int x = x0; x < std::min(x0 + terrainChunkCells, cellsX); ++x) {
                                this->addCell(chunk, x, y);
                            }
                        }
                        this->scene.chunks.push_back(std::move(chunk));
                    }
                }
                if (this->missingTiles > 0) {
                    this->scene.warnings.push_back(std::to_string(this->missingTiles) +
                                                   " cells use tiles of no texture class");
                }
            }

          private:
            Image placeholder(std::uint32_t size) const {
                Image image;
                image.width = image.height = size;
                image.rgba.resize(std::size_t(size) * size * 4);
                for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
                    image.rgba[i] = 128;
                    image.rgba[i + 1] = 118;
                    image.rgba[i + 2] = 104;
                    image.rgba[i + 3] = 255;
                }
                return image;
            }

            int height(int x, int y) const {
                x = std::clamp(x, 0, this->map.width - 1);
                y = std::clamp(y, 0, this->map.height - 1);
                return this->map.heights[std::size_t(y * this->map.width + x)];
            }

            // Atlas corners (cell corner order: (x,y), (x+1,y), (x+1,y+1), (x,y+1)) of the piece of
            // `tile` the cell shows: WorldHeightMap::getUVForTileIndex. Returns true when the
            // corners were stretched for a cliff.
            bool tileCorners(int cell, int tile, float out[4][2]) {
                const int number = tile >> 2;
                const int c = number >= 0 && std::size_t(number) < this->tileClass.size()
                                  ? this->tileClass[std::size_t(number)]
                                  : -1;
                float px[4], py[4];
                bool stretched = false;
                if (c < 0) {
                    ++this->missingTiles;
                    const auto& slot = this->slots.back();
                    px[0] = px[3] = float(slot.x);
                    px[1] = px[2] = float(slot.x + 32);
                    py[2] = py[3] = float(slot.y);
                    py[0] = py[1] = float(slot.y + 32);
                } else {
                    const auto& textureClass = this->map.textureClasses[std::size_t(c)];
                    const auto& slot = this->slots[std::size_t(c)];
                    const int width = std::clamp(textureClass.width, 1, 16);
                    const int local = number - textureClass.firstTile;
                    const int column = local % width, row = (local / width) % width;  // row 0 at the image's bottom
                    const float minX = float(slot.x + column * tilePixels + ((tile & 1) ? 32 : 0));
                    const float minY = float(slot.y + (width - 1 - row) * tilePixels + ((tile & 2) ? 0 : 32));
                    px[0] = px[3] = minX;
                    px[1] = px[2] = minX + 32;
                    py[2] = py[3] = minY;
                    py[0] = py[1] = minY + 32;
                    const int cliff = this->map.cliffs.empty() ? 0 : this->map.cliffs[std::size_t(cell)];
                    if (cliff > 0) {
                        const auto& info = this->map.cliffInfos[std::size_t(cliff)];
                        const int infoNumber = info.tile >> 2;
                        const int infoClass = infoNumber >= 0 && std::size_t(infoNumber) < this->tileClass.size()
                                                  ? this->tileClass[std::size_t(infoNumber)]
                                                  : -1;
                        if (infoClass == c) {
                            const float bottom = float(slot.y + width * tilePixels);
                            for (int k = 0; k < 4; ++k) {
                                px[k] = float(slot.x) + info.uv[std::size_t(2 * k)] * cliffAtlasPixels;
                                py[k] = bottom + info.uv[std::size_t(2 * k + 1)] * cliffAtlasPixels;
                            }
                            stretched = info.flip;
                        }
                    }
                }
                for (int k = 0; k < 4; ++k) {
                    out[k][0] = px[k] / float(this->scene.atlas.width);
                    out[k][1] = py[k] / float(this->scene.atlas.height);
                }
                return stretched;
            }

            // Per-corner weight of a blend tile and whether the cell's diagonal must turn so the
            // weights interpolate along the blend (WorldHeightMap::getAlphaUVData).
            static void blendCorners(const MapBlendTile& blend, float alpha[4], bool& flip) {
                std::fill_n(alpha, 4, 0.0f);
                flip = false;
                const bool inverted = blend.inverted & 1;
                if (blend.horizontal) {
                    flip = blend.inverted & 2;
                    if (inverted) {
                        alpha[0] = alpha[3] = 1;
                    } else {
                        alpha[1] = alpha[2] = 1;
                    }
                }
                if (blend.vertical) {
                    flip = blend.inverted & 2;
                    if (inverted) {
                        alpha[0] = alpha[1] = 1;
                    } else {
                        alpha[2] = alpha[3] = 1;
                    }
                }
                if (blend.rightDiagonal) {
                    if (inverted) {
                        alpha[1] = 1;
                        if (blend.longDiagonal) {
                            alpha[0] = alpha[2] = 1;
                        }
                    } else {
                        flip = true;
                        alpha[2] = 1;
                        if (blend.longDiagonal) {
                            alpha[1] = alpha[3] = 1;
                        }
                    }
                }
                if (blend.leftDiagonal) {
                    if (inverted) {
                        flip = true;
                        alpha[0] = 1;
                        if (blend.longDiagonal) {
                            alpha[1] = alpha[3] = 1;
                        }
                    } else {
                        alpha[3] = 1;
                        if (blend.longDiagonal) {
                            alpha[0] = alpha[2] = 1;
                        }
                    }
                }
                if (blend.customEdgeClass >= 0) {  // edge textures are not drawn yet
                    std::fill_n(alpha, 4, 0.0f);
                    flip = false;
                }
            }

            void addCell(TerrainChunk& chunk, int x, int y) {
                const int cell = y * this->map.width + x;
                const int cornerX[4] = {x, x + 1, x + 1, x};
                const int cornerY[4] = {y, y, y + 1, y + 1};
                float base[4][2], blend[4][2], extra[4][2];
                float blendAlpha[4] = {}, extraAlpha[4] = {};
                bool flip = false, extraFlip = false;
                bool stretched = false;
                if (this->map.hasTiles) {
                    stretched = this->tileCorners(cell, this->map.tiles[std::size_t(cell)], base);
                    std::copy_n(&base[0][0], 8, &blend[0][0]);
                    std::copy_n(&base[0][0], 8, &extra[0][0]);
                    if (const int b = this->map.blends[std::size_t(cell)]; b > 0) {
                        const auto& tile = this->map.blendTiles[std::size_t(b)];
                        stretched = this->tileCorners(cell, tile.tile, blend) || stretched;
                        blendCorners(tile, blendAlpha, flip);
                    }
                    if (const int e = this->map.extraBlends[std::size_t(cell)]; e > 0) {
                        const auto& tile = this->map.blendTiles[std::size_t(e)];
                        this->tileCorners(cell, tile.tile, extra);
                        blendCorners(tile, extraAlpha, extraFlip);
                    }
                } else {
                    for (int k = 0; k < 4; ++k) {
                        base[k][0] = blend[k][0] = extra[k][0] = 0;
                        base[k][1] = blend[k][1] = extra[k][1] = 0;
                    }
                }
                const int h0 = this->height(x, y), h1 = this->height(x + 1, y);
                const int h2 = this->height(x + 1, y + 1), h3 = this->height(x, y + 1);
                const bool blended = std::any_of(blendAlpha,
                                                 blendAlpha + 4,
                                                 [](float a) {
                                                     return a > 0;
                                                 }) ||
                                     std::any_of(extraAlpha, extraAlpha + 4, [](float a) {
                                         return a > 0;
                                     });
                if (stretched || !blended) {
                    // Cliffs (as the engine does) and plain cells: the diagonal over the smaller height step.
                    flip = std::abs(h0 - h2) > std::abs(h1 - h3);
                } else if (!flip) {
                    flip = extraFlip;
                }

                const auto first = std::uint32_t(chunk.vertices.size());
                for (int k = 0; k < 4; ++k) {
                    const int gx = cornerX[k], gy = cornerY[k];
                    TerrainVertex vertex{};
                    vertex.position[0] = float(gx - this->map.border) * mapXYFactor;
                    vertex.position[1] = float(gy - this->map.border) * mapXYFactor;
                    vertex.position[2] = float(this->height(gx, gy)) * mapHeightScale;
                    const float dx = float(this->height(gx + 1, gy) - this->height(gx - 1, gy)) * mapHeightScale;
                    const float dy = float(this->height(gx, gy + 1) - this->height(gx, gy - 1)) * mapHeightScale;
                    const float nx = -dx, ny = -dy, nz = 2 * mapXYFactor;
                    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                    vertex.normal[0] = nx / length;
                    vertex.normal[1] = ny / length;
                    vertex.normal[2] = nz / length;
                    std::copy_n(base[k], 2, vertex.uv);
                    std::copy_n(blend[k], 2, vertex.blendUv);
                    std::copy_n(extra[k], 2, vertex.extraUv);
                    vertex.alpha[0] = blendAlpha[k];
                    vertex.alpha[1] = extraAlpha[k];
                    chunk.vertices.push_back(vertex);
                }
                // HeightMap.cpp: triangles (0,2,3) (0,1,2); a flipped cell turns the corners by one.
                const std::uint32_t order[2][6] = {{0, 2, 3, 0, 1, 2}, {1, 3, 0, 1, 2, 3}};
                for (const auto index: order[flip ? 1 : 0]) {
                    chunk.indices.push_back(first + index);
                }
            }

            const MapFile& map;
            const GameFiles& files;
            MapScene& scene;
            std::vector<AtlasSlot> slots;  // per texture class, then the placeholder
            std::vector<int> tileClass;
            int missingTiles = 0;
        };

        float cross(const std::array<float, 2>& o, const std::array<float, 2>& a, const std::array<float, 2>& b) {
            return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
        }

        // Player color (0xRRGGBB) of the side that owns `team`, white when none.
        std::uint32_t ownerColor(const MapFile& map, const std::string& team) {
            if (team.empty()) {
                return 0xFFFFFF;
            }
            std::string owner;
            for (const auto& dict: map.teams) {
                const auto* name = findDictValue(dict, "teamName");
                const auto* teamOwner = findDictValue(dict, "teamOwner");
                if (name && teamOwner && name->text() == team) {
                    owner = teamOwner->text();
                    break;
                }
            }
            for (const auto& side: map.sides) {
                const auto* name = findDictValue(side.player, "playerName");
                const auto* color = findDictValue(side.player, "playerColor");
                if (name && color && name->text() == owner && color->integer()) {
                    return std::uint32_t(*color->integer()) & 0xFFFFFF;
                }
            }
            return 0xFFFFFF;
        }
    }

    std::vector<std::uint32_t> triangulatePolygon(const std::vector<std::array<float, 2>>& points) {
        std::vector<std::uint32_t> result;
        if (points.size() < 3) {
            return result;
        }
        float area = 0;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto& a = points[i];
            const auto& b = points[(i + 1) % points.size()];
            area += a[0] * b[1] - b[0] * a[1];
        }
        std::vector<std::uint32_t> left(points.size());
        for (std::size_t i = 0; i < left.size(); ++i) {
            left[i] = std::uint32_t(area >= 0 ? i : left.size() - 1 - i);  // counter-clockwise
        }
        const auto inside = [&](const auto& p, const auto& a, const auto& b, const auto& c) {
            return cross(a, b, p) >= 0 && cross(b, c, p) >= 0 && cross(c, a, p) >= 0;
        };
        std::size_t guard = 0;
        std::size_t i = 0;
        while (left.size() > 3 && guard < left.size() * 2) {
            const auto n = left.size();
            const auto ia = left[(i + n - 1) % n], ib = left[i % n], ic = left[(i + 1) % n];
            const auto &a = points[ia], &b = points[ib], &c = points[ic];
            bool ear = cross(a, b, c) > 0;
            for (std::size_t k = 0; ear && k < n; ++k) {
                const auto ip = left[k];
                if (ip != ia && ip != ib && ip != ic && inside(points[ip], a, b, c)) {
                    ear = false;
                }
            }
            if (ear) {
                result.insert(result.end(), {ia, ib, ic});
                left.erase(left.begin() + std::ptrdiff_t(i % n));
                guard = 0;
                if (i >= left.size()) {
                    i = 0;
                }
            } else {
                i = (i + 1) % n;
                ++guard;
            }
        }
        // Three left, or an outline that cannot be clipped: close it with a fan.
        for (std::size_t k = 1; k + 1 < left.size(); ++k) {
            result.insert(result.end(), {left[0], left[k], left[k + 1]});
        }
        return result;
    }

    Image shrinkImage(Image image, std::uint32_t maxSize) {
        while ((image.width > maxSize || image.height > maxSize) && image.width >= 2 && image.height >= 2) {
            Image half;
            half.width = image.width / 2;
            half.height = image.height / 2;
            half.rgba.resize(std::size_t(half.width) * half.height * 4);
            for (std::uint32_t y = 0; y < half.height; ++y) {
                for (std::uint32_t x = 0; x < half.width; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        const auto at = [&](std::uint32_t sx, std::uint32_t sy) {
                            return unsigned(image.rgba[(std::size_t(sy) * image.width + sx) * 4 + std::size_t(c)]);
                        };
                        const unsigned sum =
                            at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1);
                        half.rgba[(std::size_t(y) * half.width + x) * 4 + std::size_t(c)] = std::uint8_t((sum + 2) / 4);
                    }
                }
            }
            image = std::move(half);
        }
        return image;
    }

    bool readGameTexture(const GameFiles& files,
                         const std::string& folder,
                         const std::string& name,
                         Image& out,
                         std::string& error) {
        const auto file = lower(name);
        const auto dot = file.rfind('.');
        const auto stem = folder + (dot == std::string::npos ? file : file.substr(0, dot));
        for (const auto* extension: {".dds", ".tga"}) {
            std::string bytes;
            if (!files.read(stem + extension, bytes)) {
                continue;
            }
            const bool ok =
                std::string_view(extension) == ".dds" ? decodeDds(bytes, out, error) : decodeTga(bytes, out, error);
            if (ok) {
                return true;
            }
            error = name + ": " + error;
            return false;
        }
        error = name + " not found";
        return false;
    }

    PlacedObject resolveObject(const MapFile& map, const MapObject& object, const ObjectLookup& findObject) {
        PlacedObject placed;
        placed.templateName = object.templateName;
        placed.x = object.x;
        placed.y = object.y;
        placed.z = groundHeight(map, object.x, object.y) + object.z;
        placed.angle = object.angle;
        placed.flags = object.flags;
        if (const auto* owner = findDictValue(object.properties, "originalOwner")) {
            placed.owner = owner->text();
        }
        placed.color = ownerColor(map, placed.owner);
        const auto* info = findObject ? findObject(object.templateName) : nullptr;
        if (object.flags & mapObjectRoadFlags) {
            placed.kind = "road";
        } else if (object.flags & mapObjectBridgeFlags) {
            placed.kind = "bridge";
        } else if (findDictValue(object.properties, "waypointID")) {
            placed.kind = "waypoint";
        } else if (info && !info->model.empty()) {
            placed.kind = "model";
            placed.model = info->model;
            placed.muzzleFlashes = info->muzzleFlashes;
            placed.hiddenSubObjects = info->hiddenSubObjects;
        } else {
            placed.kind = "marker";
        }
        return placed;
    }

    MapScene buildMapScene(const MapFile& map, const GameFiles& files) {
        MapScene scene;
        TerrainBuilder terrain(map, files, scene);
        terrain.buildAtlas();
        terrain.buildChunks();

        if (!map.boundaries.empty()) {
            scene.playable = {0,
                              0,
                              float(map.boundaries[0][0]) * mapXYFactor,
                              float(map.boundaries[0][1]) * mapXYFactor};
        }

        // Water.
        std::string text;
        if (files.read("data/ini/water.ini", text)) {
            scene.waterLook = parseWaterLook(text, map.lighting.timeOfDay);
        }
        std::string error;
        for (const auto* name: {&scene.waterLook.standingTexture, &scene.waterLook.texture}) {
            if (!name->empty() && readGameTexture(files, "art/textures/", *name, scene.waterTexture, error)) {
                scene.waterTexture = shrinkImage(std::move(scene.waterTexture), 512);
                break;
            }
        }
        for (const auto& polygon: map.polygons) {
            if ((!polygon.water && !polygon.river) || polygon.points.size() < 3) {
                continue;
            }
            WaterArea area;
            area.name = polygon.name;
            area.river = polygon.river;
            area.height = float(polygon.points.front()[2]);
            std::vector<std::array<float, 2>> outline;
            outline.reserve(polygon.points.size());
            area.vertices.reserve(polygon.points.size() * 3);
            for (const auto& point: polygon.points) {
                outline.push_back({float(point[0]), float(point[1])});
                area.vertices.insert(area.vertices.end(),
                                     {float(point[0]), float(point[1]), polygon.river ? float(point[2]) : area.height});
            }
            area.indices = triangulatePolygon(outline);
            scene.water.push_back(std::move(area));
        }
        return scene;
    }
}
