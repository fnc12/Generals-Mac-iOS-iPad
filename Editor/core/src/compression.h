#pragma once

#include <string>
#include <string_view>

namespace end_editor {

    // The engine's compressed file wrapper (CompressionManager): a 4-byte tag, the uncompressed
    // size (uint32, little endian), then the payload. Maps use "EAR\0" (EA RefPack) and, rarely,
    // "ZL1\0".."ZL9\0" (zlib; only when the core is built with zlib).
    bool isCompressedGameData(std::string_view bytes);

    // Unpacks `bytes` into `out`; data without a known tag is copied as is. Returns false and
    // sets `error` for damaged or unsupported data.
    bool decompressGameData(std::string_view bytes, std::string& out, std::string& error);

    // RefPack stream (the payload after the 8-byte wrapper, starting with the 0x10FB-style
    // header). Bounds checked: a damaged stream fails instead of reading or writing outside.
    bool decodeRefPack(std::string_view stream, std::string& out, std::string& error);
}
