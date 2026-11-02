#include "cast/cast_channel.h"

namespace ap::cast {

namespace {

void write_varint(std::vector<uint8_t>& buf, uint64_t val) {
    while (val >= 0x80) {
        buf.push_back(static_cast<uint8_t>((val & 0x7F) | 0x80));
        val >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(val));
}

void write_tag(std::vector<uint8_t>& buf, uint32_t field, uint32_t wire_type) {
    write_varint(buf, (field << 3) | wire_type);
}

void write_string(std::vector<uint8_t>& buf, uint32_t field, const std::string& str) {
    write_tag(buf, field, 2);
    write_varint(buf, str.size());
    buf.insert(buf.end(), str.begin(), str.end());
}

void write_bytes(std::vector<uint8_t>& buf, uint32_t field, const std::vector<uint8_t>& bytes) {
    write_tag(buf, field, 2);
    write_varint(buf, bytes.size());
    buf.insert(buf.end(), bytes.begin(), bytes.end());
}

} // namespace

std::vector<uint8_t> encode_framed_message(const CastMessage& msg) {
    std::vector<uint8_t> payload;
    payload.reserve(256 + msg.payload_utf8.size() + msg.payload_binary.size());

    // Field 1: protocol_version (varint)
    write_tag(payload, 1, 0);
    write_varint(payload, msg.protocol_version);

    // Field 2: source_id (string)
    if (!msg.source_id.empty()) {
        write_string(payload, 2, msg.source_id);
    }

    // Field 3: destination_id (string)
    if (!msg.destination_id.empty()) {
        write_string(payload, 3, msg.destination_id);
    }

    // Field 4: namespace (string)
    if (!msg.ns.empty()) {
        write_string(payload, 4, msg.ns);
    }

    // Field 5: payload_type (varint)
    write_tag(payload, 5, 0);
    write_varint(payload, msg.payload_type);

    // Field 6: payload_utf8 (string)
    if (!msg.payload_utf8.empty()) {
        write_string(payload, 6, msg.payload_utf8);
    }

    // Field 7: payload_binary (bytes)
    if (!msg.payload_binary.empty()) {
        write_bytes(payload, 7, msg.payload_binary);
    }

    // 4-byte big endian header
    uint32_t len = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> framed;
    framed.reserve(4 + len);
    framed.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
    framed.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
    framed.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    framed.push_back(static_cast<uint8_t>(len & 0xFF));
    framed.insert(framed.end(), payload.begin(), payload.end());

    return framed;
}

bool decode_cast_message(const uint8_t* data, size_t len, CastMessage& out) {
    out = CastMessage{};
    size_t i = 0;
    while (i < len) {
        uint64_t key = 0;
        int shift = 0;
        while (i < len) {
            uint8_t b = data[i++];
            key |= static_cast<uint64_t>(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80)) break;
        }

        uint32_t field = static_cast<uint32_t>(key >> 3);
        uint32_t wire  = static_cast<uint32_t>(key & 7);

        if (wire == 0) {
            uint64_t val = 0;
            shift = 0;
            while (i < len) {
                uint8_t b = data[i++];
                val |= static_cast<uint64_t>(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80)) break;
            }
            if (field == 1) out.protocol_version = static_cast<int>(val);
            else if (field == 5) out.payload_type = static_cast<int>(val);
        } else if (wire == 2) {
            uint64_t chunk_len = 0;
            shift = 0;
            while (i < len) {
                uint8_t b = data[i++];
                chunk_len |= static_cast<uint64_t>(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80)) break;
            }
            if (i + chunk_len > len) return false;

            if (field == 2) {
                out.source_id.assign(reinterpret_cast<const char*>(data + i), chunk_len);
            } else if (field == 3) {
                out.destination_id.assign(reinterpret_cast<const char*>(data + i), chunk_len);
            } else if (field == 4) {
                out.ns.assign(reinterpret_cast<const char*>(data + i), chunk_len);
            } else if (field == 6) {
                out.payload_utf8.assign(reinterpret_cast<const char*>(data + i), chunk_len);
            } else if (field == 7) {
                out.payload_binary.assign(data + i, data + i + chunk_len);
            }
            i += static_cast<size_t>(chunk_len);
        } else {
            // Unsupported wire type
            return false;
        }
    }
    return true;
}

} // namespace ap::cast
