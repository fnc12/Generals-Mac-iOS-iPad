#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace end_editor {

    // Thrown when a chunk file is damaged: a read past the end of its chunk, an unknown name id...
    struct ChunkError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // A value of the engine's Dict (key -> bool / int / real / ASCII / Unicode string).
    struct DictValue {
        enum Type : std::uint8_t { boolean = 0, integer = 1, real = 2, ascii = 3, unicode = 4 } type = boolean;
        std::string key;
        std::int32_t intValue = 0;  // bool and int
        float realValue = 0;
        std::string text;  // ASCII, or Unicode converted to UTF-8

        // The value as the inspector shows it ("true", "42", "1.5", the text).
        std::string toString() const;
    };
    using Dict = std::vector<DictValue>;

    const DictValue* findDictValue(const Dict& dict, std::string_view key);
    const char* dictTypeName(DictValue::Type type);

    // Reader of the engine's DataChunk files (DataChunk.cpp): a "CkMp" table of contents that maps
    // name ids to names, then chunks of {uint32 name id, uint16 version, int32 size, data}.
    // Chunks nest: a parent's data can hold child chunks. Values are little endian.
    class DataChunkReader {
      public:
        // Reads the table of contents. Throws ChunkError when there is none.
        explicit DataChunkReader(std::string_view bytes);

        struct Chunk {
            std::string name;
            std::uint16_t version = 0;
            std::size_t start = 0;  // first data byte (the 10-byte header is just before it)
            std::size_t end = 0;  // one past the last data byte
        };
        static constexpr std::size_t headerBytes = 10;  // name id, version, size

        // Opens the next chunk inside the current one (or at the top level). Returns false at the end.
        bool openChunk(Chunk& chunk);
        // Skips what is left of the innermost open chunk and closes it.
        void closeChunk();
        std::size_t left() const;
        // Number of open chunks; closeChunksTo() closes the inner ones (after an error inside them).
        std::size_t depth() const {
            return this->open.size();
        }
        void closeChunksTo(std::size_t depth);
        bool atEndOfChunk() const {
            return this->left() == 0;
        }

        std::int32_t readInt();
        float readReal();
        std::uint8_t readByte();
        std::string readBytes(std::size_t count);
        std::string readAsciiString();
        std::string readUnicodeString();  // UTF-16 in the file, UTF-8 here
        Dict readDict();

        const std::string& nameOf(std::uint32_t id) const;
        // The table of contents (name id -> name) and where it ends (the first chunk header).
        const std::map<std::uint32_t, std::string>& nameTable() const {
            return this->names;
        }
        std::size_t tableEnd() const {
            return this->tocEnd;
        }

      private:
        void need(std::size_t count) const;

        std::string_view data;
        std::size_t position = 0;
        std::size_t tocEnd = 0;
        std::map<std::uint32_t, std::string> names;
        std::vector<Chunk> open;
    };
}
