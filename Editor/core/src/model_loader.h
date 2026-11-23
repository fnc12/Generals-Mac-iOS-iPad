#pragma once

#include "game_files.h"
#include "texture.h"
#include "w3d.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace end_editor {

    // One vertex as the client's renderer takes it (36 bytes, no padding).
    struct ModelVertex {
        float position[3];
        float normal[3];
        float uv[2];  // already flipped to the top-left texture origin
        std::uint32_t bone;
    };
    static_assert(sizeof(ModelVertex) == 36);

    // A piece of a mesh drawn with one texture. Every vertex names the bone that moves it:
    // all the same bone for rigid meshes, per vertex for skins.
    struct ModelPart {
        std::string mesh;
        std::string texture;  // key into LoadedModel::textures, "" when untextured
        std::uint32_t bone = 0;
        bool skinned = false;
        bool twoSided = false;
        bool hidden = false;  // hidden by default (damage states, muzzle flashes...)
        std::vector<ModelVertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    struct ModelAnimationInfo {
        std::string name;  // what INI files write after the dot: "AVTANK_SKL.AVTANK_FIRE" -> "AVTANK_FIRE"
        std::string file;  // archive path of the file that holds it
        std::uint32_t frameCount = 0;
        std::uint32_t frameRate = 30;
    };

    struct LoadedModel {
        std::string name;
        std::string error;  // set when nothing can be shown
        W3dHierarchy hierarchy;
        std::vector<ModelPart> parts;
        std::map<std::string, Image> textures;
        std::vector<ModelAnimationInfo> animations;
        std::array<float, 3> boundsMin{}, boundsMax{};  // of the bind pose
        std::vector<std::string> warnings;
    };

    // Bone matrices of every frame of one animation, frame after frame.
    struct ModelPose {
        std::uint32_t frameCount = 1;
        std::uint32_t frameRate = 30;
        std::vector<Matrix4> matrices;      // frameCount * bones
        std::vector<std::uint8_t> visible;  // frameCount * bones
    };

    // Builds previewable models from the game's W3D files. Keeps the parsed files of the
    // current model so that switching animations does not read them again.
    class ModelLoader {
      public:
        explicit ModelLoader(const GameFiles &files) : files(files) {}

        LoadedModel load(const std::string &modelName);

        // Pose of `model` for an animation from model.animations; the bind pose (one frame)
        // when `animationName` is empty or unknown.
        ModelPose pose(const LoadedModel &model, const std::string &animationName, std::string &error);

        static std::string modelPath(const std::string &name);

      private:
        const W3dFile *parsedFile(const std::string &path);
        void indexAnimationFiles();
        std::string findTexture(const std::string &textureName) const;

        const GameFiles &files;
        std::map<std::string, std::unique_ptr<W3dFile>> cache;  // path -> parsed (or empty when missing)
        bool animationsIndexed = false;
        std::multimap<std::string, std::string> animationFilesByHierarchy;  // lower-case hierarchy -> path
    };
}
