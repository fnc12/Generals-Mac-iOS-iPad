#pragma once

// Reader of Westwood 3D (.w3d) files: meshes, bone hierarchies, HLOD models and animations.
// The format is the one in the engine's w3d_file.h; the parts the engine never uses for
// Zero Hour assets (adaptive-delta animations, morph animations, emitters...) are skipped.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace end_editor {

    namespace w3d {
        constexpr std::uint32_t meshFlagHidden = 0x00001000;
        constexpr std::uint32_t meshFlagTwoSided = 0x00002000;
        constexpr std::uint32_t meshGeometryMask = 0x00FF0000;
        constexpr std::uint32_t meshGeometrySkin = 0x00020000;
    }

    // How a mesh is drawn, reduced from W3dShaderStruct's blend functions and alpha test.
    enum class W3dBlend : std::uint8_t { opaque = 0, alphaTest = 1, alphaBlend = 2, additive = 3, multiply = 4 };

    // `shader` is one 16-byte W3dShaderStruct.
    W3dBlend blendOfShader(std::string_view shader);

    struct W3dMesh {
        std::string name;       // "TURRET"
        std::string container;  // "AVTANK" (the model it belongs to)
        std::uint32_t attributes = 0;
        std::vector<float> positions;        // 3 per vertex; in the space of the bone it is attached to
        std::vector<float> normals;          // 3 per vertex
        std::vector<float> uvs;              // 2 per vertex, empty when untextured
        std::vector<std::uint16_t> boneIds;  // 1 per vertex, skins only
        std::vector<std::uint32_t> indices;  // 3 per triangle
        std::vector<std::string> textures;   // texture file names ("avtank.tga")
        std::vector<std::uint32_t> triangleTextures;  // per triangle index into `textures`; empty = all 0
        std::vector<W3dBlend> shaders;                // of the first pass
        std::vector<std::uint32_t> triangleShaders;   // per triangle index into `shaders`; empty = all 0

        std::string fullName() const {
            return this->container.empty() ? this->name : this->container + "." + this->name;
        }
        bool isSkin() const {
            return (this->attributes & w3d::meshGeometryMask) == w3d::meshGeometrySkin;
        }
        std::size_t vertexCount() const {
            return this->positions.size() / 3;
        }
    };

    struct W3dPivot {
        std::string name;
        int parent = -1;
        std::array<float, 3> translation{};
        std::array<float, 4> rotation{0, 0, 0, 1};  // quaternion x, y, z, w
    };

    struct W3dHierarchy {
        std::string name;
        std::vector<W3dPivot> pivots;
    };

    struct W3dSubObject {
        std::uint32_t bone = 0;
        std::string name;  // "CONTAINER.MESH"
    };

    // A model: which meshes hang on which bones. Only the most detailed LOD is kept, plus the
    // aggregates that are shown with every LOD.
    struct W3dHlod {
        std::string name;
        std::string hierarchy;
        std::vector<W3dSubObject> subObjects;
    };

    // An animation decoded to one value per frame. Per pivot: `translation[p]` holds 3 floats
    // per frame, `rotation[p]` 4 (quaternion), `visible[p]` 1 byte; an empty vector means the
    // animation does not move (or hide) that pivot.
    struct W3dAnimation {
        std::string name;
        std::string hierarchy;
        std::uint32_t frameCount = 0;
        std::uint32_t frameRate = 30;
        std::vector<std::vector<float>> translation;
        std::vector<std::vector<float>> rotation;
        std::vector<std::vector<std::uint8_t>> visible;
    };

    struct W3dFile {
        std::vector<W3dMesh> meshes;
        std::vector<W3dHierarchy> hierarchies;
        std::vector<W3dHlod> hlods;
        std::vector<W3dAnimation> animations;
        std::vector<std::string> warnings;
    };

    W3dFile parseW3d(std::string_view bytes);

    // What the first chunk of a file says it is, read from its first bytes (see
    // w3dHeaderPeekSize). For an animation, `hierarchy` names the skeleton it moves.
    struct W3dFileKind {
        enum Kind { otherFile, meshFile, hierarchyFile, animationFile, hlodFile } kind = otherFile;
        std::string hierarchy;
    };
    constexpr std::uint32_t w3dHeaderPeekSize = 64;
    W3dFileKind peekW3dKind(std::string_view firstBytes);

    // Column-major 4x4 matrix (the layout Metal and OpenGL use).
    using Matrix4 = std::array<float, 16>;

    // World transform of every pivot at `frame` (bind pose when `animation` is null), as the
    // engine computes it: world = parent world * base pose * translation * rotation.
    // `visible` (optional) receives one 0/1 byte per pivot.
    std::vector<Matrix4> poseHierarchy(const W3dHierarchy &hierarchy, const W3dAnimation *animation,
                                       std::uint32_t frame, std::vector<std::uint8_t> *visible = nullptr);
}
