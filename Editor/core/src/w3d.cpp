#include "w3d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace end_editor {

    namespace {
        // Chunk ids from w3d_file.h.
        enum : std::uint32_t {
            chunkMesh = 0x000,
            chunkVertices = 0x002,
            chunkVertexNormals = 0x003,
            chunkVertexInfluences = 0x00E,
            chunkMeshHeader3 = 0x01F,
            chunkTriangles = 0x020,
            chunkTextures = 0x030,
            chunkTexture = 0x031,
            chunkTextureName = 0x032,
            chunkMaterialPass = 0x038,
            chunkTextureStage = 0x048,
            chunkTextureIds = 0x049,
            chunkStageTexcoords = 0x04A,
            chunkHierarchy = 0x100,
            chunkHierarchyHeader = 0x101,
            chunkPivots = 0x102,
            chunkAnimation = 0x200,
            chunkAnimationHeader = 0x201,
            chunkAnimationChannel = 0x202,
            chunkBitChannel = 0x203,
            chunkCompressedAnimation = 0x280,
            chunkCompressedAnimationHeader = 0x281,
            chunkCompressedAnimationChannel = 0x282,
            chunkCompressedBitChannel = 0x283,
            chunkHlod = 0x700,
            chunkHlodHeader = 0x701,
            chunkHlodLodArray = 0x702,
            chunkHlodSubObject = 0x704,
            chunkHlodAggregateArray = 0x705,
        };

        enum : std::uint16_t { channelX = 0, channelY = 1, channelZ = 2, channelQ = 6 };
        constexpr std::uint16_t flavorTimeCoded = 0;
        constexpr std::uint32_t timeCodeFlag = 0x80000000u;
        constexpr std::size_t nameLength = 16;

        // All targets (x64, arm64) are little endian, like the files.
        template<class T>
        T get(std::string_view data, std::size_t offset) {
            T value{};
            if(offset + sizeof(T) <= data.size()) {
                std::memcpy(&value, data.data() + offset, sizeof(T));
            }
            return value;
        }

        std::string getName(std::string_view data, std::size_t offset, std::size_t length) {
            if(offset >= data.size()) {
                return {};
            }
            const auto field = data.substr(offset, length);
            return std::string(field.substr(0, field.find('\0')));
        }

        struct Chunk {
            std::uint32_t type = 0;
            std::string_view data;
        };

        // Splits `data` into chunks. Returns false (and adds a warning) when a chunk overruns.
        template<class F>
        bool forEachChunk(std::string_view data, std::vector<std::string> &warnings, F &&visit) {
            std::size_t offset = 0;
            while(offset + 8 <= data.size()) {
                const auto type = get<std::uint32_t>(data, offset);
                const auto size = get<std::uint32_t>(data, offset + 4) & 0x7FFFFFFFu;  // top bit: has sub-chunks
                if(offset + 8 + std::uint64_t(size) > data.size()) {
                    char text[64];
                    std::snprintf(text, sizeof(text), "chunk 0x%X overruns its parent", unsigned(type));
                    warnings.push_back(text);
                    return false;
                }
                visit(Chunk{type, data.substr(offset + 8, size)});
                offset += 8 + std::size_t(size);
            }
            return true;
        }

        template<class T>
        std::vector<T> getArray(std::string_view data, std::size_t count, std::size_t stride = sizeof(T)) {
            count = std::min(count, data.size() / stride);
            std::vector<T> result(count);
            for(std::size_t i = 0; i < count; ++i) {
                result[i] = get<T>(data, i * stride);
            }
            return result;
        }

        W3dMesh parseMesh(std::string_view data, std::vector<std::string> &warnings) {
            W3dMesh mesh;
            std::uint32_t triangleCount = 0;
            bool passSeen = false;
            forEachChunk(data, warnings, [&](const Chunk &chunk) {
                switch(chunk.type) {
                    case chunkMeshHeader3:
                        mesh.attributes = get<std::uint32_t>(chunk.data, 4);
                        mesh.name = getName(chunk.data, 8, nameLength);
                        mesh.container = getName(chunk.data, 24, nameLength);
                        triangleCount = get<std::uint32_t>(chunk.data, 40);
                        break;
                    case chunkVertices:
                        mesh.positions = getArray<float>(chunk.data, chunk.data.size() / 4);
                        break;
                    case chunkVertexNormals:
                        mesh.normals = getArray<float>(chunk.data, chunk.data.size() / 4);
                        break;
                    case chunkVertexInfluences:
                        mesh.boneIds = getArray<std::uint16_t>(chunk.data, chunk.data.size() / 8, 8);
                        break;
                    case chunkTriangles: {
                        const std::size_t count = chunk.data.size() / 32;
                        mesh.indices.resize(count * 3);
                        for(std::size_t i = 0; i < count; ++i) {
                            for(std::size_t k = 0; k < 3; ++k) {
                                mesh.indices[i * 3 + k] = get<std::uint32_t>(chunk.data, i * 32 + k * 4);
                            }
                        }
                        break;
                    }
                    case chunkTextures:
                        forEachChunk(chunk.data, warnings, [&](const Chunk &texture) {
                            if(texture.type != chunkTexture) {
                                return;
                            }
                            std::string name;
                            forEachChunk(texture.data, warnings, [&](const Chunk &field) {
                                if(field.type == chunkTextureName) {
                                    name = getName(field.data, 0, field.data.size());
                                }
                            });
                            mesh.textures.push_back(name);
                        });
                        break;
                    case chunkMaterialPass:
                        // The preview draws the first pass and its first texture stage only.
                        if(passSeen) {
                            break;
                        }
                        passSeen = true;
                        forEachChunk(chunk.data, warnings, [&](const Chunk &pass) {
                            if(pass.type != chunkTextureStage || !mesh.uvs.empty()) {
                                return;
                            }
                            forEachChunk(pass.data, warnings, [&](const Chunk &stage) {
                                if(stage.type == chunkTextureIds) {
                                    mesh.triangleTextures = getArray<std::uint32_t>(stage.data, stage.data.size() / 4);
                                } else if(stage.type == chunkStageTexcoords) {
                                    mesh.uvs = getArray<float>(stage.data, stage.data.size() / 4);
                                }
                            });
                        });
                        break;
                    default:
                        break;
                }
            });

            // Drop what does not fit together instead of letting a renderer read out of bounds.
            const std::size_t vertexCount = mesh.positions.size() / 3;
            mesh.positions.resize(vertexCount * 3);
            if(mesh.normals.size() != vertexCount * 3) {
                mesh.normals.assign(vertexCount * 3, 0.0f);
            }
            if(mesh.uvs.size() != vertexCount * 2) {
                mesh.uvs.clear();
            }
            if(mesh.boneIds.size() != vertexCount) {
                mesh.boneIds.clear();
            }
            const bool badIndex = std::any_of(mesh.indices.begin(), mesh.indices.end(),
                                              [&](std::uint32_t index) { return index >= vertexCount; });
            if(badIndex) {
                warnings.push_back("mesh " + mesh.fullName() + ": triangle index out of range");
                mesh.indices.clear();
            }
            const std::size_t triangles = mesh.indices.size() / 3;
            if(triangleCount != 0 && triangleCount != triangles && !badIndex) {
                warnings.push_back("mesh " + mesh.fullName() + ": triangle count differs from the header");
            }
            if(mesh.triangleTextures.size() == 1) {
                const std::uint32_t only = mesh.triangleTextures[0];  // a copy: assign() reallocates
                mesh.triangleTextures.assign(triangles, only);
            }
            if(mesh.triangleTextures.size() != triangles) {
                mesh.triangleTextures.clear();
            }
            return mesh;
        }

        W3dHierarchy parseHierarchy(std::string_view data, std::vector<std::string> &warnings) {
            W3dHierarchy hierarchy;
            forEachChunk(data, warnings, [&](const Chunk &chunk) {
                if(chunk.type == chunkHierarchyHeader) {
                    hierarchy.name = getName(chunk.data, 4, nameLength);
                } else if(chunk.type == chunkPivots) {
                    constexpr std::size_t stride = 60;  // W3dPivotStruct
                    for(std::size_t offset = 0; offset + stride <= chunk.data.size(); offset += stride) {
                        W3dPivot pivot;
                        pivot.name = getName(chunk.data, offset, nameLength);
                        const auto parent = get<std::uint32_t>(chunk.data, offset + 16);
                        pivot.parent = parent == 0xFFFFFFFFu ? -1 : int(parent);
                        for(std::size_t k = 0; k < 3; ++k) {
                            pivot.translation[k] = get<float>(chunk.data, offset + 20 + k * 4);
                        }
                        // offset + 32: Euler angles, the same orientation as the quaternion
                        for(std::size_t k = 0; k < 4; ++k) {
                            pivot.rotation[k] = get<float>(chunk.data, offset + 44 + k * 4);
                        }
                        hierarchy.pivots.push_back(std::move(pivot));
                    }
                }
            });
            return hierarchy;
        }

        void addSubObjects(std::string_view data, std::vector<W3dSubObject> &out, std::vector<std::string> &warnings) {
            forEachChunk(data, warnings, [&](const Chunk &chunk) {
                if(chunk.type == chunkHlodSubObject) {
                    out.push_back(W3dSubObject{get<std::uint32_t>(chunk.data, 0), getName(chunk.data, 4, nameLength * 2)});
                }
            });
        }

        W3dHlod parseHlod(std::string_view data, std::vector<std::string> &warnings) {
            W3dHlod hlod;
            std::string_view topLod;
            std::vector<W3dSubObject> aggregates;
            forEachChunk(data, warnings, [&](const Chunk &chunk) {
                if(chunk.type == chunkHlodHeader) {
                    hlod.name = getName(chunk.data, 8, nameLength);
                    hlod.hierarchy = getName(chunk.data, 24, nameLength);
                } else if(chunk.type == chunkHlodLodArray) {
                    topLod = chunk.data;  // arrays go from the least to the most detailed
                } else if(chunk.type == chunkHlodAggregateArray) {
                    addSubObjects(chunk.data, aggregates, warnings);
                }
            });
            addSubObjects(topLod, hlod.subObjects, warnings);
            hlod.subObjects.insert(hlod.subObjects.end(), aggregates.begin(), aggregates.end());
            return hlod;
        }

        // Makes the per-pivot arrays of `animation` large enough for `pivot`.
        void reservePivot(W3dAnimation &animation, std::size_t pivot) {
            if(animation.translation.size() <= pivot) {
                animation.translation.resize(pivot + 1);
                animation.rotation.resize(pivot + 1);
                animation.visible.resize(pivot + 1);
            }
        }

        std::vector<float> &translationOf(W3dAnimation &animation, std::size_t pivot) {
            reservePivot(animation, pivot);
            auto &values = animation.translation[pivot];
            if(values.empty()) {
                values.assign(std::size_t(animation.frameCount) * 3, 0.0f);
            }
            return values;
        }

        std::vector<float> &rotationOf(W3dAnimation &animation, std::size_t pivot) {
            reservePivot(animation, pivot);
            auto &values = animation.rotation[pivot];
            if(values.empty()) {
                values.resize(std::size_t(animation.frameCount) * 4);
                for(std::size_t f = 0; f < animation.frameCount; ++f) {
                    values[f * 4 + 3] = 1.0f;
                }
            }
            return values;
        }

        std::vector<std::uint8_t> &visibleOf(W3dAnimation &animation, std::size_t pivot) {
            reservePivot(animation, pivot);
            auto &values = animation.visible[pivot];
            if(values.empty()) {
                values.assign(animation.frameCount, 1);
            }
            return values;
        }

        // Writes `count` floats for one frame into the right place of a channel.
        void storeFrame(W3dAnimation &animation, std::uint16_t type, std::size_t pivot, std::uint32_t frame,
                        const float *values, std::size_t count) {
            if(type == channelQ && count == 4) {
                auto &target = rotationOf(animation, pivot);
                std::copy(values, values + 4, target.begin() + std::ptrdiff_t(frame) * 4);
            } else if(type <= channelZ && count == 1) {
                translationOf(animation, pivot)[std::size_t(frame) * 3 + type] = values[0];
            }
            // Euler channels (XR, YR, ZR) are not used by the engine's raw animations.
        }

        void parseRawChannel(W3dAnimation &animation, std::string_view data, std::vector<std::string> &warnings) {
            const auto first = get<std::uint16_t>(data, 0);
            const auto last = get<std::uint16_t>(data, 2);
            const auto vectorLength = get<std::uint16_t>(data, 4);
            const auto type = get<std::uint16_t>(data, 6);
            const auto pivot = get<std::uint16_t>(data, 8);
            if(last < first || vectorLength == 0 || vectorLength > 4) {
                return;
            }
            // The exporter sometimes wrote extra data at the end, so the size is checked, not matched.
            const std::size_t frames = std::size_t(last - first) + 1;
            if(12 + frames * vectorLength * 4 > data.size()) {
                warnings.push_back("animation " + animation.name + ": short channel");
                return;
            }
            if(type != channelQ && type > channelZ) {
                return;
            }
            // Frames outside [first, last] keep the identity the arrays start with.
            if(type == channelQ) {
                rotationOf(animation, pivot);
            } else {
                translationOf(animation, pivot);
            }
            for(std::size_t i = 0; i < frames && first + i < animation.frameCount; ++i) {
                float values[4];
                for(std::size_t k = 0; k < vectorLength; ++k) {
                    values[k] = get<float>(data, 12 + (i * vectorLength + k) * 4);
                }
                storeFrame(animation, type, pivot, std::uint32_t(first + i), values, vectorLength);
            }
        }

        void parseRawBitChannel(W3dAnimation &animation, std::string_view data) {
            const auto first = get<std::uint16_t>(data, 0);
            const auto last = get<std::uint16_t>(data, 2);
            const auto pivot = get<std::uint16_t>(data, 6);
            const auto defaultValue = get<std::uint8_t>(data, 8);
            auto &visible = visibleOf(animation, pivot);
            for(std::uint32_t frame = 0; frame < animation.frameCount; ++frame) {
                std::uint8_t value = defaultValue ? 1 : 0;
                if(frame >= first && frame <= last) {
                    const std::size_t bit = frame - first;
                    value = (get<std::uint8_t>(data, 9 + bit / 8) >> (bit % 8)) & 1;
                }
                visible[frame] = value;
            }
        }

        // A time-coded channel: packets of [time code, value...]; values between two packets are
        // interpolated unless the second packet has the "binary movement" flag.
        void parseTimeCodedChannel(W3dAnimation &animation, std::string_view data, std::vector<std::string> &warnings) {
            const auto count = get<std::uint32_t>(data, 0);
            const auto pivot = get<std::uint16_t>(data, 4);
            const auto vectorLength = get<std::uint8_t>(data, 6);
            const auto type = get<std::uint8_t>(data, 7);
            if(count == 0 || vectorLength == 0 || vectorLength > 4) {
                return;
            }
            const std::size_t packet = std::size_t(vectorLength) + 1;
            if(8 + std::uint64_t(count) * packet * 4 > data.size()) {
                warnings.push_back("animation " + animation.name + ": short time-coded channel");
                return;
            }
            if(type != channelQ && type > channelZ) {
                return;
            }
            const auto timeAt = [&](std::size_t i) { return get<std::uint32_t>(data, 8 + i * packet * 4) & ~timeCodeFlag; };
            const auto flagAt = [&](std::size_t i) { return (get<std::uint32_t>(data, 8 + i * packet * 4) & timeCodeFlag) != 0; };
            const auto valueAt = [&](std::size_t i, std::size_t k) { return get<float>(data, 8 + (i * packet + 1 + k) * 4); };

            std::size_t index = 0;
            for(std::uint32_t frame = 0; frame < animation.frameCount; ++frame) {
                while(index + 1 < count && timeAt(index + 1) <= frame) {
                    ++index;
                }
                float values[4];
                const bool hold = index + 1 >= count || flagAt(index + 1) || frame <= timeAt(index);
                if(hold) {
                    for(std::size_t k = 0; k < vectorLength; ++k) {
                        values[k] = valueAt(index, k);
                    }
                } else {
                    const float t0 = float(timeAt(index));
                    const float t1 = float(timeAt(index + 1));
                    const float ratio = t1 > t0 ? (float(frame) - t0) / (t1 - t0) : 0.0f;
                    float dot = 0.0f;
                    for(std::size_t k = 0; k < vectorLength; ++k) {
                        dot += valueAt(index, k) * valueAt(index + 1, k);
                    }
                    // Quaternions take the short way round.
                    const float sign = (type == channelQ && dot < 0.0f) ? -1.0f : 1.0f;
                    float length = 0.0f;
                    for(std::size_t k = 0; k < vectorLength; ++k) {
                        values[k] = valueAt(index, k) + (sign * valueAt(index + 1, k) - valueAt(index, k)) * ratio;
                        length += values[k] * values[k];
                    }
                    if(type == channelQ && length > 0.0f) {
                        const float scale = 1.0f / std::sqrt(length);
                        for(auto &value: values) {
                            value *= scale;
                        }
                    }
                }
                storeFrame(animation, type, pivot, frame, values, vectorLength);
            }
        }

        void parseTimeCodedBitChannel(W3dAnimation &animation, std::string_view data) {
            const auto count = get<std::uint32_t>(data, 0);
            const auto pivot = get<std::uint16_t>(data, 4);
            if(count == 0 || 8 + std::uint64_t(count) * 4 > data.size()) {
                return;
            }
            auto &visible = visibleOf(animation, pivot);
            std::size_t index = 0;
            for(std::uint32_t frame = 0; frame < animation.frameCount; ++frame) {
                while(index + 1 < count && (get<std::uint32_t>(data, 8 + (index + 1) * 4) & ~timeCodeFlag) <= frame) {
                    ++index;
                }
                visible[frame] = (get<std::uint32_t>(data, 8 + index * 4) & timeCodeFlag) ? 1 : 0;
            }
        }

        W3dAnimation parseAnimation(std::string_view data, bool compressed, std::vector<std::string> &warnings) {
            W3dAnimation animation;
            std::uint16_t flavor = flavorTimeCoded;
            forEachChunk(data, warnings, [&](const Chunk &chunk) {
                switch(chunk.type) {
                    case chunkAnimationHeader:
                    case chunkCompressedAnimationHeader:
                        animation.name = getName(chunk.data, 4, nameLength);
                        animation.hierarchy = getName(chunk.data, 20, nameLength);
                        animation.frameCount = std::min<std::uint32_t>(get<std::uint32_t>(chunk.data, 36), 100000);
                        if(compressed) {
                            animation.frameRate = get<std::uint16_t>(chunk.data, 40);
                            flavor = get<std::uint16_t>(chunk.data, 42);
                            if(flavor != flavorTimeCoded) {
                                warnings.push_back("animation " + animation.name +
                                                   ": adaptive-delta compression is not supported");
                            }
                        } else {
                            animation.frameRate = get<std::uint32_t>(chunk.data, 40);
                        }
                        break;
                    case chunkAnimationChannel:
                        parseRawChannel(animation, chunk.data, warnings);
                        break;
                    case chunkBitChannel:
                        parseRawBitChannel(animation, chunk.data);
                        break;
                    case chunkCompressedAnimationChannel:
                        if(flavor == flavorTimeCoded) {
                            parseTimeCodedChannel(animation, chunk.data, warnings);
                        }
                        break;
                    case chunkCompressedBitChannel:
                        if(flavor == flavorTimeCoded) {
                            parseTimeCodedBitChannel(animation, chunk.data);
                        }
                        break;
                    default:
                        break;
                }
            });
            if(animation.frameRate == 0) {
                animation.frameRate = 30;
            }
            return animation;
        }

        Matrix4 identity() {
            return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        }

        Matrix4 multiply(const Matrix4 &a, const Matrix4 &b) {
            Matrix4 r{};
            for(int column = 0; column < 4; ++column) {
                for(int row = 0; row < 4; ++row) {
                    float sum = 0.0f;
                    for(int k = 0; k < 4; ++k) {
                        sum += a[k * 4 + row] * b[column * 4 + k];
                    }
                    r[column * 4 + row] = sum;
                }
            }
            return r;
        }

        // Translation followed by rotation: T(t) * R(q).
        Matrix4 translateRotate(const float *t, const float *q) {
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            Matrix4 m = identity();
            m[0] = 1 - 2 * (y * y + z * z);
            m[1] = 2 * (x * y + z * w);
            m[2] = 2 * (x * z - y * w);
            m[4] = 2 * (x * y - z * w);
            m[5] = 1 - 2 * (x * x + z * z);
            m[6] = 2 * (y * z + x * w);
            m[8] = 2 * (x * z + y * w);
            m[9] = 2 * (y * z - x * w);
            m[10] = 1 - 2 * (x * x + y * y);
            m[12] = t[0];
            m[13] = t[1];
            m[14] = t[2];
            return m;
        }
    }

    W3dFile parseW3d(std::string_view bytes) {
        W3dFile file;
        forEachChunk(bytes, file.warnings, [&](const Chunk &chunk) {
            switch(chunk.type) {
                case chunkMesh:
                    file.meshes.push_back(parseMesh(chunk.data, file.warnings));
                    break;
                case chunkHierarchy:
                    file.hierarchies.push_back(parseHierarchy(chunk.data, file.warnings));
                    break;
                case chunkHlod:
                    file.hlods.push_back(parseHlod(chunk.data, file.warnings));
                    break;
                case chunkAnimation:
                    file.animations.push_back(parseAnimation(chunk.data, false, file.warnings));
                    break;
                case chunkCompressedAnimation:
                    file.animations.push_back(parseAnimation(chunk.data, true, file.warnings));
                    break;
                default:
                    break;
            }
        });
        return file;
    }

    W3dFileKind peekW3dKind(std::string_view firstBytes) {
        W3dFileKind result;
        if(firstBytes.size() < 8) {
            return result;
        }
        const auto type = get<std::uint32_t>(firstBytes, 0);
        switch(type) {
            case chunkMesh:
                result.kind = W3dFileKind::meshFile;
                break;
            case chunkHierarchy:
                result.kind = W3dFileKind::hierarchyFile;
                break;
            case chunkHlod:
                result.kind = W3dFileKind::hlodFile;
                break;
            case chunkAnimation:
            case chunkCompressedAnimation:
                result.kind = W3dFileKind::animationFile;
                // first sub-chunk is the header: 8 bytes of chunk header, then version, name
                result.hierarchy = getName(firstBytes, 8 + 8 + 20, nameLength);
                break;
            default:
                break;
        }
        return result;
    }

    std::vector<Matrix4> poseHierarchy(const W3dHierarchy &hierarchy, const W3dAnimation *animation,
                                       std::uint32_t frame, std::vector<std::uint8_t> *visible) {
        const std::size_t count = hierarchy.pivots.size();
        std::vector<Matrix4> world(count, identity());
        if(visible) {
            visible->assign(count, 1);
        }
        if(animation && animation->frameCount > 0) {
            frame = std::min(frame, animation->frameCount - 1);
        } else {
            animation = nullptr;
        }
        // Pivot 0 is the root: the engine puts the object's own transform there.
        for(std::size_t p = 1; p < count; ++p) {
            const auto &pivot = hierarchy.pivots[p];
            const Matrix4 parent = pivot.parent >= 0 && std::size_t(pivot.parent) < p ? world[std::size_t(pivot.parent)]
                                                                                       : identity();
            Matrix4 local = translateRotate(pivot.translation.data(), pivot.rotation.data());
            if(animation && p < animation->translation.size()) {
                const float zero[3] = {0, 0, 0};
                const float none[4] = {0, 0, 0, 1};
                const auto &t = animation->translation[p];
                const auto &q = animation->rotation[p];
                local = multiply(local, translateRotate(t.empty() ? zero : &t[std::size_t(frame) * 3],
                                                        q.empty() ? none : &q[std::size_t(frame) * 4]));
                if(visible && !animation->visible[p].empty()) {
                    (*visible)[p] = animation->visible[p][frame];
                }
            }
            world[p] = multiply(parent, local);
        }
        return world;
    }
}
