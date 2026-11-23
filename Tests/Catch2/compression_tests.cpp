// GeneralsX @build fnc12 21/11/2026 Round trips through CompressionManager (maps, saves and
// replays use it). Data is generated here; no game assets are needed.
#include <catch2/catch_all.hpp>

#include "Compression.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {
    std::vector<std::uint8_t> makeSample(std::size_t size) {
        // Repetitive enough to compress, varied enough to exercise the encoders.
        std::vector<std::uint8_t> data(size);
        std::uint32_t x = 12345;
        for(std::size_t i = 0; i < size; ++i) {
            x = x * 1103515245u + 12345u;
            data[i] = (i % 64 < 48) ? std::uint8_t('A' + (i % 7)) : std::uint8_t(x >> 24);
        }
        return data;
    }
}

TEST_CASE("CompressionManager round trip") {
    // NONE is "not compressed" and has its own test below.
    auto type = GENERATE(COMPRESSION_REFPACK,
                         COMPRESSION_NOXLZH,
                         COMPRESSION_ZLIB1,
                         COMPRESSION_ZLIB5,
                         COMPRESSION_ZLIB9,
                         COMPRESSION_BTREE,
                         COMPRESSION_HUFF);
    auto size = GENERATE(std::size_t(1), std::size_t(100), std::size_t(64 * 1024));
    CAPTURE(CompressionManager::getCompressionNameByType(type), size);

    if(type == COMPRESSION_NOXLZH && size < 2) {
        SKIP("LZHL refuses 1-byte input (compressData returns 0), a refusal, not corrupt data");
    }

    auto original = makeSample(size);
    std::vector<std::uint8_t> packed(CompressionManager::getMaxCompressedSize(Int(size), type));
    const Int packedLen = CompressionManager::compressData(type, original.data(), Int(size), packed.data(), Int(packed.size()));
    REQUIRE(packedLen > 0);

    REQUIRE(CompressionManager::isDataCompressed(packed.data(), packedLen));
    REQUIRE(CompressionManager::getCompressionType(packed.data(), packedLen) == type);
    REQUIRE(CompressionManager::getUncompressedSize(packed.data(), packedLen) == Int(size));

    std::vector<std::uint8_t> unpacked(size);
    const Int unpackedLen = CompressionManager::decompressData(packed.data(), packedLen, unpacked.data(), Int(size));
    REQUIRE(unpackedLen == Int(size));
    REQUIRE(unpacked == original);
}

TEST_CASE("RefPack header uses the retail (big-endian) layout") {
    // Maps and network map transfers use RefPack (getPreferredCompression). The stream after the
    // 8-byte "EAR\0" + size header must start with 0x10FB and a 3-byte big-endian size, as written
    // by the Windows game; otherwise other builds cannot read what this build writes.
    const std::size_t size = 0x012345;
    auto original = makeSample(size);
    std::vector<std::uint8_t> packed(CompressionManager::getMaxCompressedSize(Int(size), COMPRESSION_REFPACK));
    const Int packedLen = CompressionManager::compressData(COMPRESSION_REFPACK, original.data(), Int(size), packed.data(), Int(packed.size()));
    REQUIRE(packedLen > 13);
    REQUIRE(std::string(reinterpret_cast<const char*>(packed.data()), 3) == "EAR");
    CHECK(packed[8] == 0x10);
    CHECK(packed[9] == 0xFB);
    CHECK(packed[10] == 0x01);
    CHECK(packed[11] == 0x23);
    CHECK(packed[12] == 0x45);
}

TEST_CASE("CompressionManager plain data is not compressed") {
    const std::string text = "not a compressed block";
    REQUIRE_FALSE(CompressionManager::isDataCompressed(text.data(), Int(text.size())));
    REQUIRE(CompressionManager::getCompressionType(text.data(), Int(text.size())) == COMPRESSION_NONE);
}

TEST_CASE("Data that does not compress fits getMaxCompressedSize") {
    // The EAC encoders get no output size: getMaxCompressedSize is the only thing that keeps them
    // inside the buffer. A guard area after the buffer must stay untouched.
    auto type = GENERATE(COMPRESSION_REFPACK,
                         COMPRESSION_NOXLZH,
                         COMPRESSION_ZLIB1,
                         COMPRESSION_ZLIB9,
                         COMPRESSION_BTREE,
                         COMPRESSION_HUFF);
    auto size = GENERATE(std::size_t(1), std::size_t(7), std::size_t(113), std::size_t(4096), std::size_t(300 * 1000));
    CAPTURE(CompressionManager::getCompressionNameByType(type), size);

    if(type == COMPRESSION_NOXLZH && size < 2) {
        SKIP("LZHL refuses 1-byte input");
    }

    std::mt19937 engine{std::uint32_t(size)};
    std::vector<std::uint8_t> original(size);
    std::generate(original.begin(), original.end(), [&engine] { return std::uint8_t(engine()); });

    const std::size_t maxLen = std::size_t(CompressionManager::getMaxCompressedSize(Int(size), type));
    const std::size_t guard = 64 * 1024;
    std::vector<std::uint8_t> packed(maxLen + guard, 0xCD);
    const Int packedLen = CompressionManager::compressData(type, original.data(), Int(size), packed.data(), Int(maxLen));
    REQUIRE(packedLen > 0);
    REQUIRE(std::size_t(packedLen) <= maxLen);
    REQUIRE(std::all_of(packed.begin() + maxLen, packed.end(), [](std::uint8_t b) { return b == 0xCD; }));

    std::vector<std::uint8_t> unpacked(size);
    REQUIRE(CompressionManager::decompressData(packed.data(), packedLen, unpacked.data(), Int(size)) == Int(size));
    REQUIRE(unpacked == original);
}
