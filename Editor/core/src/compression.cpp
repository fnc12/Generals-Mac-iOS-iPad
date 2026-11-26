#include "compression.h"

#include <cstdint>
#include <cstring>

#if END_EDITOR_HAS_ZLIB
#include <zlib.h>
#endif

namespace end_editor {

    namespace {
        std::uint32_t readLittleEndian32(std::string_view bytes, std::size_t at) {
            return std::uint32_t(std::uint8_t(bytes[at])) | (std::uint32_t(std::uint8_t(bytes[at + 1])) << 8) |
                   (std::uint32_t(std::uint8_t(bytes[at + 2])) << 16) |
                   (std::uint32_t(std::uint8_t(bytes[at + 3])) << 24);
        }

        bool isZlibTag(std::string_view tag) {
            return tag.size() == 4 && tag[0] == 'Z' && tag[1] == 'L' && tag[2] >= '1' && tag[2] <= '9' && tag[3] == '\0';
        }

        // Larger results are not maps (the biggest the game allows is a few tens of MB).
        constexpr std::uint32_t maxUncompressedSize = 256u * 1024 * 1024;
    }

    bool isCompressedGameData(std::string_view bytes) {
        if(bytes.size() < 8) {
            return false;
        }
        const auto tag = bytes.substr(0, 4);
        return tag == std::string_view("EAR\0", 4) || isZlibTag(tag);
    }

    bool decodeRefPack(std::string_view stream, std::string &out, std::string &error) {
        const auto *s = reinterpret_cast<const std::uint8_t *>(stream.data());
        const std::size_t length = stream.size();
        std::size_t in = 0;
        const auto need = [&](std::size_t count) { return in + count <= length; };
        if(!need(2)) {
            error = "RefPack: no header";
            return false;
        }
        const unsigned type = (unsigned(s[0]) << 8) | s[1];
        in = 2;
        if((type & 0x00FF) != 0xFB) {
            error = "RefPack: bad header";
            return false;
        }
        const std::size_t sizeBytes = (type & 0x8000) ? 4 : 3;
        if(type & 0x100) {
            in += sizeBytes;  // the compressed size, not needed
        }
        if(!need(sizeBytes)) {
            error = "RefPack: no size";
            return false;
        }
        std::uint32_t size = 0;
        for(std::size_t i = 0; i < sizeBytes; ++i) {
            size = (size << 8) | s[in++];
        }
        if(size > maxUncompressedSize) {
            error = "RefPack: implausible size";
            return false;
        }
        out.assign(size, '\0');
        auto *d = reinterpret_cast<std::uint8_t *>(out.data());
        std::size_t at = 0;

        // Copies `count` literal bytes, then `run` bytes from `distance` back (overlapping on purpose).
        const auto emit = [&](std::size_t count, std::size_t distance, std::size_t run) {
            if(!need(count) || at + count + run > size || distance > at + count) {
                return false;
            }
            std::memcpy(d + at, s + in, count);
            in += count;
            at += count;
            for(std::size_t i = 0; i < run; ++i, ++at) {
                d[at] = d[at - distance];
            }
            return true;
        };

        while(true) {
            if(!need(1)) {
                error = "RefPack: stream ends early";
                return false;
            }
            const unsigned first = s[in++];
            bool ok = true;
            if(!(first & 0x80)) {  // 2 bytes: up to 3 literals, copy 3..10 from up to 1024 back
                if(!need(1)) {
                    break;
                }
                const unsigned second = s[in++];
                ok = emit(first & 3, (((first & 0x60) << 3) | second) + 1, ((first & 0x1C) >> 2) + 3);
            } else if(!(first & 0x40)) {  // 3 bytes: copy 4..67 from up to 16384 back
                if(!need(2)) {
                    break;
                }
                const unsigned second = s[in++];
                const unsigned third = s[in++];
                ok = emit(second >> 6, (((second & 0x3F) << 8) | third) + 1, (first & 0x3F) + 4);
            } else if(!(first & 0x20)) {  // 4 bytes: copy 5..1028 from up to 131072 back
                if(!need(3)) {
                    break;
                }
                const unsigned second = s[in++];
                const unsigned third = s[in++];
                const unsigned fourth = s[in++];
                ok = emit(first & 3, ((((first & 0x10) >> 4) << 16) | (second << 8) | third) + 1,
                          (((first & 0x0C) >> 2) << 8) + fourth + 5);
            } else {
                const std::size_t literals = ((first & 0x1F) << 2) + 4;
                if(literals <= 112) {
                    ok = emit(literals, 0, 0);
                } else {  // end of stream, with 0..3 last literals
                    if(!emit(first & 3, 0, 0)) {
                        break;
                    }
                    if(at != size) {
                        error = "RefPack: " + std::to_string(at) + " of " + std::to_string(size) + " bytes";
                        return false;
                    }
                    return true;
                }
            }
            if(!ok) {
                error = "RefPack: damaged stream";
                return false;
            }
        }
        error = "RefPack: stream ends early";
        return false;
    }

    bool decompressGameData(std::string_view bytes, std::string &out, std::string &error) {
        if(!isCompressedGameData(bytes)) {
            out.assign(bytes);
            return true;
        }
        const auto tag = bytes.substr(0, 4);
        const std::uint32_t size = readLittleEndian32(bytes, 4);
        if(size > maxUncompressedSize) {
            error = "implausible uncompressed size";
            return false;
        }
        if(tag == std::string_view("EAR\0", 4)) {
            if(!decodeRefPack(bytes.substr(8), out, error)) {
                return false;
            }
            if(out.size() != size) {
                error = "RefPack: size does not match the header";
                return false;
            }
            return true;
        }
#if END_EDITOR_HAS_ZLIB
        out.assign(size, '\0');
        uLongf outLength = size;
        const int result = uncompress(reinterpret_cast<Bytef *>(out.data()), &outLength,
                                      reinterpret_cast<const Bytef *>(bytes.data() + 8), uLong(bytes.size() - 8));
        if(result != Z_OK || outLength != size) {
            error = "zlib: damaged data";
            return false;
        }
        return true;
#else
        error = "zlib compressed data (this build has no zlib)";
        return false;
#endif
    }
}
