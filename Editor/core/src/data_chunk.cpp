#include "data_chunk.h"

#include <algorithm>
#include <cstring>

namespace end_editor {

    namespace {
        void appendUtf8(std::string &out, std::uint32_t c) {
            if(c < 0x80) {
                out.push_back(char(c));
            } else if(c < 0x800) {
                out.push_back(char(0xC0 | (c >> 6)));
                out.push_back(char(0x80 | (c & 0x3F)));
            } else {
                out.push_back(char(0xE0 | (c >> 12)));
                out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
                out.push_back(char(0x80 | (c & 0x3F)));
            }
        }
    }

    std::string DictValue::toString() const {
        switch(this->type) {
            case boolean:
                return this->intValue ? "true" : "false";
            case integer:
                return std::to_string(this->intValue);
            case real: {
                std::string text = std::to_string(this->realValue);
                // "1.500000" -> "1.5"
                while(text.size() > 1 && text.back() == '0' && text.find('.') != std::string::npos) {
                    text.pop_back();
                }
                if(!text.empty() && text.back() == '.') {
                    text.pop_back();
                }
                return text;
            }
            case ascii:
            case unicode:
                return this->text;
        }
        return {};
    }

    const DictValue *findDictValue(const Dict &dict, std::string_view key) {
        for(const auto &value: dict) {
            if(value.key == key) {
                return &value;
            }
        }
        return nullptr;
    }

    const char *dictTypeName(DictValue::Type type) {
        switch(type) {
            case DictValue::boolean:
                return "bool";
            case DictValue::integer:
                return "int";
            case DictValue::real:
                return "real";
            case DictValue::ascii:
                return "string";
            case DictValue::unicode:
                return "unicode";
        }
        return "?";
    }

    DataChunkReader::DataChunkReader(std::string_view bytes) : data(bytes) {
        if(bytes.size() < 8 || bytes.substr(0, 4) != "CkMp") {
            throw ChunkError("not a chunk file (no CkMp header)");
        }
        this->position = 4;
        const auto count = this->readInt();
        if(count < 0 || std::size_t(count) > bytes.size()) {
            throw ChunkError("bad table of contents");
        }
        for(std::int32_t i = 0; i < count; ++i) {
            const auto length = this->readByte();
            auto name = this->readBytes(length);
            const auto id = std::uint32_t(this->readInt());
            this->names[id] = std::move(name);
        }
    }

    const std::string &DataChunkReader::nameOf(std::uint32_t id) const {
        const auto found = this->names.find(id);
        if(found == this->names.end()) {
            throw ChunkError("unknown name id " + std::to_string(id));
        }
        return found->second;
    }

    std::size_t DataChunkReader::left() const {
        const std::size_t end = this->open.empty() ? this->data.size() : this->open.back().end;
        return end > this->position ? end - this->position : 0;
    }

    void DataChunkReader::need(std::size_t count) const {
        if(count > this->left()) {
            throw ChunkError(this->open.empty() ? std::string("file ends early")
                                                : "read past the end of chunk " + this->open.back().name);
        }
    }

    bool DataChunkReader::openChunk(Chunk &chunk) {
        constexpr std::size_t headerBytes = 10;
        if(this->left() < headerBytes) {
            return false;
        }
        const auto id = std::uint32_t(this->readInt());
        std::uint16_t version = 0;
        std::memcpy(&version, this->data.data() + this->position, 2);
        this->position += 2;
        const auto size = this->readInt();
        if(size < 0 || std::size_t(size) > this->left()) {
            throw ChunkError("chunk " + std::to_string(id) + " is larger than its parent");
        }
        chunk.name = this->nameOf(id);
        chunk.version = version;
        chunk.start = this->position;
        chunk.end = this->position + std::size_t(size);
        this->open.push_back(chunk);
        return true;
    }

    void DataChunkReader::closeChunk() {
        if(this->open.empty()) {
            return;
        }
        this->position = std::max(this->position, this->open.back().end);
        this->open.pop_back();
    }

    void DataChunkReader::closeChunksTo(std::size_t depth) {
        while(this->open.size() > depth) {
            this->closeChunk();
        }
    }

    std::int32_t DataChunkReader::readInt() {
        this->need(4);
        std::int32_t value = 0;
        std::memcpy(&value, this->data.data() + this->position, 4);
        this->position += 4;
        return value;
    }

    float DataChunkReader::readReal() {
        this->need(4);
        float value = 0;
        std::memcpy(&value, this->data.data() + this->position, 4);
        this->position += 4;
        return value;
    }

    std::uint8_t DataChunkReader::readByte() {
        this->need(1);
        return std::uint8_t(this->data[this->position++]);
    }

    std::string DataChunkReader::readBytes(std::size_t count) {
        this->need(count);
        std::string result(this->data.substr(this->position, count));
        this->position += count;
        return result;
    }

    std::string DataChunkReader::readAsciiString() {
        this->need(2);
        std::uint16_t length = 0;
        std::memcpy(&length, this->data.data() + this->position, 2);
        this->position += 2;
        return this->readBytes(length);
    }

    std::string DataChunkReader::readUnicodeString() {
        this->need(2);
        std::uint16_t length = 0;
        std::memcpy(&length, this->data.data() + this->position, 2);
        this->position += 2;
        const auto raw = this->readBytes(std::size_t(length) * 2);
        std::string result;
        for(std::size_t i = 0; i + 1 < raw.size(); i += 2) {
            std::uint32_t c = std::uint8_t(raw[i]) | (std::uint32_t(std::uint8_t(raw[i + 1])) << 8);
            if(c >= 0xD800 && c <= 0xDFFF) {
                c = 0xFFFD;  // the game has no surrogate pairs
            }
            appendUtf8(result, c);
        }
        return result;
    }

    Dict DataChunkReader::readDict() {
        this->need(2);
        std::uint16_t count = 0;
        std::memcpy(&count, this->data.data() + this->position, 2);
        this->position += 2;
        Dict dict;
        dict.reserve(count);
        for(std::uint16_t i = 0; i < count; ++i) {
            const auto keyAndType = std::uint32_t(this->readInt());
            DictValue value;
            value.key = this->nameOf(keyAndType >> 8);
            switch(keyAndType & 0xFF) {
                case DictValue::boolean:
                    value.type = DictValue::boolean;
                    value.intValue = this->readByte() ? 1 : 0;
                    break;
                case DictValue::integer:
                    value.type = DictValue::integer;
                    value.intValue = this->readInt();
                    break;
                case DictValue::real:
                    value.type = DictValue::real;
                    value.realValue = this->readReal();
                    break;
                case DictValue::ascii:
                    value.type = DictValue::ascii;
                    value.text = this->readAsciiString();
                    break;
                case DictValue::unicode:
                    value.type = DictValue::unicode;
                    value.text = this->readUnicodeString();
                    break;
                default:
                    throw ChunkError("unknown dictionary value type " + std::to_string(keyAndType & 0xFF));
            }
            dict.push_back(std::move(value));
        }
        return dict;
    }
}
