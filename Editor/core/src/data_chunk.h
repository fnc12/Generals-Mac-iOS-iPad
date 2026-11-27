#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace end_editor {

    // Thrown when a chunk file is damaged: a read past the end of its chunk, an unknown name id...
    struct ChunkError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // The two string kinds of a Dict value: ASCII, and Unicode (UTF-16 in the file, UTF-8 here).
    struct AsciiText {
        std::string text;
        bool operator==(const AsciiText&) const = default;
    };
    struct UnicodeText {
        std::string text;
        bool operator==(const UnicodeText&) const = default;
    };

    // A value of the engine's Dict (key -> bool / int / real / ASCII / Unicode string).
    struct DictValue {
        // In the order of the file's type codes (Dict::DataType: DICT_BOOL, DICT_INT, DICT_REAL,
        // DICT_ASCIISTRING, DICT_UNICODESTRING), so a value's code is its index.
        using Data = std::variant<bool, std::int32_t, float, AsciiText, UnicodeText>;

        std::string key;
        Data data;

        // The type code the file stores.
        std::uint8_t typeCode() const {
            return std::uint8_t(this->data.index());
        }
        // An int value, nullptr for the other types.
        const std::int32_t* integer() const {
            return std::get_if<std::int32_t>(&this->data);
        }
        // The text of a string value (either kind), "" for the other types.
        std::string_view text() const;
        // The value as the inspector shows it ("true", "42", "1.5", the text).
        std::string toString() const;
    };
    using Dict = std::vector<DictValue>;

    const DictValue* findDictValue(const Dict& dict, std::string_view key);
    // "bool" | "int" | "real" | "string" | "unicode": the type as the store and the commands name it.
    const char* dictTypeName(const DictValue& value);

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
