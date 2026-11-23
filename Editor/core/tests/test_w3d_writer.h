#pragma once

// Builds small W3D files, DDS and TGA images for tests, so no game data is needed. The layouts
// follow w3d_file.h; the contents are made up.

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace end_editor_tests {

    struct Bytes {
        std::string data;

        Bytes &u8(std::uint8_t v) {
            this->data.push_back(char(v));
            return *this;
        }
        Bytes &u16(std::uint16_t v) {
            return this->raw(&v, 2);
        }
        Bytes &u32(std::uint32_t v) {
            return this->raw(&v, 4);
        }
        Bytes &f32(float v) {
            return this->raw(&v, 4);
        }
        Bytes &floats(std::initializer_list<float> values) {
            for(float v: values) {
                this->f32(v);
            }
            return *this;
        }
        // Fixed-size, zero-padded name field.
        Bytes &name(const std::string &text, std::size_t length) {
            std::string field = text.substr(0, length);
            field.resize(length, '\0');
            this->data += field;
            return *this;
        }
        Bytes &raw(const void *bytes, std::size_t size) {
            this->data.append(static_cast<const char *>(bytes), size);
            return *this;
        }
        Bytes &append(const std::string &more) {
            this->data += more;
            return *this;
        }
    };

    // A chunk: type, size (top bit set when it holds sub-chunks), body.
    inline std::string chunk(std::uint32_t type, const std::string &body, bool container = false) {
        Bytes out;
        out.u32(type).u32(std::uint32_t(body.size()) | (container ? 0x80000000u : 0u));
        out.data += body;
        return out.data;
    }

    inline std::string container(std::uint32_t type, std::initializer_list<std::string> children) {
        std::string body;
        for(const auto &child: children) {
            body += child;
        }
        return chunk(type, body, true);
    }

    // A W3dShaderStruct with the given blend functions (w3d_file.h enums) and alpha test.
    inline std::string shader(std::uint8_t sourceBlend, std::uint8_t destinationBlend, bool alphaTest) {
        std::string bytes(16, '\0');
        bytes[1] = 1;  // depth writes on
        bytes[3] = char(destinationBlend);
        bytes[7] = char(sourceBlend);
        bytes[12] = alphaTest ? 1 : 0;
        return bytes;
    }
    inline std::string opaqueShader() {
        return shader(1, 0, false);
    }

    // A textured square of two triangles in the XY plane.
    inline std::string squareMesh(const std::string &meshName, const std::string &containerName,
                                  const std::string &texture, std::uint32_t attributes = 0,
                                  const std::string &shaderBytes = opaqueShader()) {
        Bytes header;
        header.u32(0x00040002).u32(attributes).name(meshName, 16).name(containerName, 16);
        header.u32(2).u32(4);  // triangles, vertices
        header.data.append(116 - header.data.size(), '\0');
        Bytes vertices, normals, triangles, uvs;
        vertices.floats({0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0});
        normals.floats({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});
        for(const auto &tri: {std::vector<std::uint32_t>{0, 1, 2}, std::vector<std::uint32_t>{0, 2, 3}}) {
            triangles.u32(tri[0]).u32(tri[1]).u32(tri[2]).u32(0).floats({0, 0, 1, 0});
        }
        uvs.floats({0, 0, 1, 0, 1, 1, 0, 1});
        Bytes textureId, shaderId;
        textureId.u32(0);  // one id for every triangle
        shaderId.u32(0);
        return container(
            0x000, {chunk(0x01F, header.data), chunk(0x002, vertices.data), chunk(0x003, normals.data),
                    chunk(0x020, triangles.data),
                    chunk(0x029, shaderBytes),
                    container(0x030, {container(0x031, {chunk(0x032, texture + std::string(1, '\0'))})}),
                    container(0x038, {chunk(0x03A, shaderId.data),
                                      container(0x048, {chunk(0x049, textureId.data), chunk(0x04A, uvs.data)})})});
    }

    struct TestPivot {
        std::string name;
        std::uint32_t parent;
        float translation[3];
        float rotation[4];
    };

    inline std::string hierarchy(const std::string &hierarchyName, const std::vector<TestPivot> &pivots) {
        Bytes header, body;
        header.u32(0x00040001).name(hierarchyName, 16).u32(std::uint32_t(pivots.size())).floats({0, 0, 0});
        for(const auto &pivot: pivots) {
            body.name(pivot.name, 16).u32(pivot.parent);
            body.floats({pivot.translation[0], pivot.translation[1], pivot.translation[2]});
            body.floats({0, 0, 0});  // Euler angles, not read
            body.floats({pivot.rotation[0], pivot.rotation[1], pivot.rotation[2], pivot.rotation[3]});
        }
        return container(0x100, {chunk(0x101, header.data), chunk(0x102, body.data)});
    }

    // One LOD with the given sub-objects (bone, "CONTAINER.MESH").
    inline std::string hlod(const std::string &modelName, const std::string &hierarchyName,
                            const std::vector<std::pair<std::uint32_t, std::string>> &subObjects) {
        Bytes header, arrayHeader;
        header.u32(0x00010000).u32(1).name(modelName, 16).name(hierarchyName, 16);
        arrayHeader.u32(std::uint32_t(subObjects.size())).f32(0);
        std::string lod = chunk(0x703, arrayHeader.data);
        for(const auto &[bone, name]: subObjects) {
            Bytes sub;
            sub.u32(bone).name(name, 32);
            lod += chunk(0x704, sub.data);
        }
        return container(0x700, {chunk(0x701, header.data), chunk(0x702, lod, true)});
    }

    inline std::string animationHeader(const std::string &animationName, const std::string &hierarchyName,
                                       std::uint32_t frames, bool compressed) {
        Bytes header;
        header.u32(0x00040001).name(animationName, 16).name(hierarchyName, 16).u32(frames);
        if(compressed) {
            header.u16(30).u16(0);  // frame rate, time-coded flavor
        } else {
            header.u32(30);
        }
        return chunk(compressed ? 0x281 : 0x201, header.data);
    }

    // Raw channel: values for frames first..last, `vectorLength` floats each.
    inline std::string rawChannel(std::uint16_t type, std::uint16_t pivot, std::uint16_t first, std::uint16_t last,
                                  std::uint16_t vectorLength, std::initializer_list<float> values) {
        Bytes body;
        body.u16(first).u16(last).u16(vectorLength).u16(type).u16(pivot).u16(0).floats(values);
        return chunk(0x202, body.data);
    }

    inline std::string rawBitChannel(std::uint16_t pivot, std::uint16_t first, std::uint16_t last,
                                     std::uint8_t defaultValue, std::uint8_t bits) {
        Bytes body;
        body.u16(first).u16(last).u16(0).u16(pivot).u8(defaultValue).u8(bits);
        return chunk(0x203, body.data);
    }

    // Time-coded channel of one float per key: (time code, value) pairs.
    inline std::string timeCodedChannel(std::uint16_t type, std::uint16_t pivot,
                                        const std::vector<std::pair<std::uint32_t, float>> &keys) {
        Bytes body;
        body.u32(std::uint32_t(keys.size())).u16(pivot).u8(1).u8(std::uint8_t(type));
        for(const auto &[time, value]: keys) {
            body.u32(time).f32(value);
        }
        return chunk(0x282, body.data);
    }

    // A 4x4 DXT1 texture of one color (RGB565).
    inline std::string solidDds(std::uint16_t color) {
        Bytes out;
        out.raw("DDS ", 4).u32(124).u32(0x1007).u32(4).u32(4).u32(8).u32(0).u32(1);
        out.data.append(44, '\0');                      // reserved
        out.u32(32).u32(4).raw("DXT1", 4);              // pixel format: size, flags (FOURCC), code
        out.data.append(128 - out.data.size(), '\0');  // rest of the header
        out.u16(color).u16(0).u32(0);                   // c0 > c1, every pixel index 0
        return out.data;
    }

    // An uncompressed 32-bit TGA, rows bottom to top as TGA stores them by default. `bgra`
    // lists the pixels from the bottom row up.
    inline std::string tga(std::uint16_t width, std::uint16_t height, const std::vector<std::uint8_t> &bgra) {
        Bytes out;
        out.u8(0).u8(0).u8(2).data.append(9, '\0');
        out.u16(width).u16(height).u8(32).u8(8);
        out.raw(bgra.data(), bgra.size());
        return out.data;
    }
}
