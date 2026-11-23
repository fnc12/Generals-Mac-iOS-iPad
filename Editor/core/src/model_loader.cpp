#include "model_loader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace end_editor {

    namespace {
        std::string lower(std::string text) {
            for(auto &c: text) {
                if(c >= 'A' && c <= 'Z') {
                    c = char(c - 'A' + 'a');
                }
            }
            return text;
        }

        bool sameName(const std::string &a, const std::string &b) {
            return lower(a) == lower(b);
        }

        bool isW3dPath(const std::string &path) {
            return path.rfind("art/w3d/", 0) == 0 && path.size() > 4 && path.compare(path.size() - 4, 4, ".w3d") == 0;
        }

        // A hierarchy with only the root, for models without bones.
        W3dHierarchy rootOnly() {
            W3dHierarchy hierarchy;
            hierarchy.pivots.push_back(W3dPivot{"ROOTTRANSFORM", -1, {}, {0, 0, 0, 1}});
            return hierarchy;
        }

        void transformPoint(const Matrix4 &m, const float *p, float *out) {
            for(int row = 0; row < 3; ++row) {
                out[row] = m[0 * 4 + row] * p[0] + m[1 * 4 + row] * p[1] + m[2 * 4 + row] * p[2] + m[12 + row];
            }
        }

        // Splits a mesh into parts by texture.
        void addMeshParts(const W3dMesh &mesh, std::uint32_t bone, std::uint32_t boneCount, std::vector<ModelPart> &parts,
                          std::vector<std::string> &warnings) {
            if(mesh.indices.empty()) {
                return;
            }
            const bool skinned = mesh.isSkin() && !mesh.boneIds.empty();
            if(mesh.isSkin() && mesh.boneIds.empty()) {
                warnings.push_back("skin " + mesh.fullName() + " has no bone links");
            }
            std::map<std::uint32_t, std::vector<std::uint32_t>> trianglesByTexture;
            const std::size_t triangleCount = mesh.indices.size() / 3;
            for(std::size_t t = 0; t < triangleCount; ++t) {
                const std::uint32_t texture = mesh.triangleTextures.empty() ? 0 : mesh.triangleTextures[t];
                trianglesByTexture[texture].push_back(std::uint32_t(t));
            }
            for(const auto &[textureIndex, triangles]: trianglesByTexture) {
                ModelPart part;
                part.mesh = mesh.fullName();
                if(!mesh.uvs.empty() && textureIndex < mesh.textures.size()) {
                    part.texture = lower(mesh.textures[textureIndex]);
                }
                part.bone = bone;
                part.skinned = skinned;
                part.twoSided = (mesh.attributes & w3d::meshFlagTwoSided) != 0;
                part.hidden = (mesh.attributes & w3d::meshFlagHidden) != 0;

                // Keep only the vertices this part uses.
                std::map<std::uint32_t, std::uint32_t> remap;
                for(const auto triangle: triangles) {
                    for(int k = 0; k < 3; ++k) {
                        const std::uint32_t source = mesh.indices[triangle * 3 + std::size_t(k)];
                        auto [it, added] = remap.emplace(source, std::uint32_t(part.vertices.size()));
                        if(added) {
                            ModelVertex vertex{};
                            std::copy_n(&mesh.positions[source * 3], 3, vertex.position);
                            std::copy_n(&mesh.normals[source * 3], 3, vertex.normal);
                            if(!mesh.uvs.empty()) {
                                vertex.uv[0] = mesh.uvs[source * 2];
                                vertex.uv[1] = 1.0f - mesh.uvs[source * 2 + 1];  // as the engine does
                            }
                            vertex.bone = skinned ? mesh.boneIds[source] : bone;
                            if(vertex.bone >= boneCount) {
                                vertex.bone = 0;
                            }
                            part.vertices.push_back(vertex);
                        }
                        part.indices.push_back(it->second);
                    }
                }
                parts.push_back(std::move(part));
            }
        }
    }

    std::string ModelLoader::modelPath(const std::string &name) {
        return "art/w3d/" + lower(name) + ".w3d";
    }

    const W3dFile *ModelLoader::parsedFile(const std::string &path) {
        auto found = this->cache.find(path);
        if(found == this->cache.end()) {
            std::unique_ptr<W3dFile> parsed;
            std::string bytes;
            if(this->files.read(path, bytes)) {
                parsed = std::make_unique<W3dFile>(parseW3d(bytes));
            }
            found = this->cache.emplace(path, std::move(parsed)).first;
        }
        return found->second.get();
    }

    void ModelLoader::indexAnimationFiles() {
        if(this->animationsIndexed) {
            return;
        }
        this->animationsIndexed = true;
        // Stand-alone animation files start with the animation chunk; reading their first
        // bytes is enough to learn which skeleton they move.
        for(const auto &path: this->files.list(isW3dPath)) {
            std::string head;
            if(!this->files.readPrefix(path, w3dHeaderPeekSize, head)) {
                continue;
            }
            const auto kind = peekW3dKind(head);
            if(kind.kind == W3dFileKind::animationFile && !kind.hierarchy.empty()) {
                this->animationFilesByHierarchy.emplace(lower(kind.hierarchy), path);
            }
        }
    }

    std::string ModelLoader::findTexture(const std::string &textureName) const {
        const auto dot = textureName.rfind('.');
        const std::string base = "art/textures/" + (dot == std::string::npos ? textureName : textureName.substr(0, dot));
        // The game prefers a .dds with the same name over the .tga the model names.
        for(const char *extension: {".dds", ".tga"}) {
            if(this->files.exists(base + extension)) {
                return base + extension;
            }
        }
        return {};
    }

    LoadedModel ModelLoader::load(const std::string &modelName) {
        this->cache.clear();
        LoadedModel model;
        model.name = modelName;
        const std::string path = modelPath(modelName);
        const W3dFile *file = this->parsedFile(path);
        if(!file) {
            model.error = "no model file " + path;
            return model;
        }
        model.warnings = file->warnings;

        const W3dHlod *hlod = nullptr;
        for(const auto &candidate: file->hlods) {
            if(!hlod || sameName(candidate.name, modelName)) {
                hlod = &candidate;
            }
        }

        if(hlod && !hlod->hierarchy.empty()) {
            const W3dHierarchy *hierarchy = nullptr;
            for(const auto &candidate: file->hierarchies) {
                if(sameName(candidate.name, hlod->hierarchy)) {
                    hierarchy = &candidate;
                }
            }
            if(!hierarchy) {
                if(const auto *other = this->parsedFile(modelPath(hlod->hierarchy))) {
                    for(const auto &candidate: other->hierarchies) {
                        if(sameName(candidate.name, hlod->hierarchy)) {
                            hierarchy = &candidate;
                        }
                    }
                }
            }
            if(hierarchy) {
                model.hierarchy = *hierarchy;
            } else {
                model.warnings.push_back("skeleton " + hlod->hierarchy + " not found");
            }
        }
        if(model.hierarchy.pivots.empty()) {
            model.hierarchy = rootOnly();
        }
        const auto boneCount = std::uint32_t(model.hierarchy.pivots.size());

        if(hlod) {
            int missing = 0;
            for(const auto &sub: hlod->subObjects) {
                const W3dMesh *mesh = nullptr;
                const auto findIn = [&](const W3dFile &in) {
                    for(const auto &candidate: in.meshes) {
                        if(sameName(candidate.fullName(), sub.name)) {
                            mesh = &candidate;
                        }
                    }
                };
                findIn(*file);
                const auto dot = sub.name.find('.');
                if(!mesh && dot != std::string::npos) {
                    if(const auto *other = this->parsedFile(modelPath(sub.name.substr(0, dot)))) {
                        findIn(*other);
                    }
                }
                if(!mesh) {
                    ++missing;  // boxes, emitters and other non-mesh objects end up here too
                    continue;
                }
                addMeshParts(*mesh, sub.bone < boneCount ? sub.bone : 0, boneCount, model.parts, model.warnings);
            }
            if(missing > 0) {
                model.warnings.push_back(std::to_string(missing) + " sub-objects are not meshes or were not found");
            }
        } else {
            for(const auto &mesh: file->meshes) {
                addMeshParts(mesh, 0, boneCount, model.parts, model.warnings);
            }
        }
        if(model.parts.empty()) {
            model.error = "the model has no meshes to show";
            return model;
        }

        // Bounds of the bind pose, for the camera.
        const auto bind = poseHierarchy(model.hierarchy, nullptr, 0);
        model.boundsMin.fill(std::numeric_limits<float>::max());
        model.boundsMax.fill(std::numeric_limits<float>::lowest());
        for(const auto &part: model.parts) {
            if(part.hidden) {
                continue;
            }
            for(const auto &vertex: part.vertices) {
                float p[3];
                transformPoint(bind[vertex.bone], vertex.position, p);
                for(int k = 0; k < 3; ++k) {
                    model.boundsMin[std::size_t(k)] = std::min(model.boundsMin[std::size_t(k)], p[k]);
                    model.boundsMax[std::size_t(k)] = std::max(model.boundsMax[std::size_t(k)], p[k]);
                }
            }
        }
        if(model.boundsMin[0] > model.boundsMax[0]) {
            model.boundsMin.fill(-1.0f);
            model.boundsMax.fill(1.0f);
        }

        std::set<std::string> textureNames;
        for(const auto &part: model.parts) {
            if(!part.texture.empty()) {
                textureNames.insert(part.texture);
            }
        }
        for(const auto &name: textureNames) {
            const auto texturePath = this->findTexture(name);
            std::string bytes, error;
            Image image;
            const bool isDds = texturePath.size() > 4 && texturePath.compare(texturePath.size() - 4, 4, ".dds") == 0;
            if(texturePath.empty() || !this->files.read(texturePath, bytes)) {
                model.warnings.push_back("texture " + name + " not found");
            } else if(!(isDds ? decodeDds(bytes, image, error) : decodeTga(bytes, image, error))) {
                model.warnings.push_back("texture " + name + ": " + error);
            } else {
                model.textures.emplace(name, std::move(image));
            }
        }

        // Animations: those in the model's own file, then stand-alone files for its skeleton.
        const auto addAnimations = [&](const W3dFile &in, const std::string &inPath) {
            for(const auto &animation: in.animations) {
                if(!sameName(animation.hierarchy, model.hierarchy.name) || animation.frameCount == 0) {
                    continue;
                }
                const bool known = std::any_of(model.animations.begin(), model.animations.end(),
                                               [&](const auto &item) { return sameName(item.name, animation.name); });
                if(!known) {
                    model.animations.push_back(
                        ModelAnimationInfo{animation.name, inPath, animation.frameCount, animation.frameRate});
                }
            }
        };
        if(!model.hierarchy.name.empty()) {
            addAnimations(*file, path);
            this->indexAnimationFiles();
            const auto range = this->animationFilesByHierarchy.equal_range(lower(model.hierarchy.name));
            for(auto it = range.first; it != range.second; ++it) {
                if(const auto *other = this->parsedFile(it->second)) {
                    addAnimations(*other, it->second);
                }
            }
        }
        return model;
    }

    ModelPose ModelLoader::pose(const LoadedModel &model, const std::string &animationName, std::string &error) {
        ModelPose result;
        const W3dAnimation *animation = nullptr;
        if(!animationName.empty()) {
            const auto info = std::find_if(model.animations.begin(), model.animations.end(),
                                           [&](const auto &item) { return sameName(item.name, animationName); });
            if(info == model.animations.end()) {
                error = "unknown animation " + animationName;
            } else if(const auto *file = this->parsedFile(info->file)) {
                for(const auto &candidate: file->animations) {
                    if(sameName(candidate.name, animationName)) {
                        animation = &candidate;
                    }
                }
            }
        }
        if(animation) {
            result.frameCount = animation->frameCount;
            result.frameRate = animation->frameRate;
        }
        const std::size_t bones = model.hierarchy.pivots.size();
        result.matrices.reserve(result.frameCount * bones);
        result.visible.reserve(result.frameCount * bones);
        std::vector<std::uint8_t> visible;
        for(std::uint32_t frame = 0; frame < result.frameCount; ++frame) {
            const auto matrices = poseHierarchy(model.hierarchy, animation, frame, &visible);
            result.matrices.insert(result.matrices.end(), matrices.begin(), matrices.end());
            result.visible.insert(result.visible.end(), visible.begin(), visible.end());
        }
        return result;
    }
}
