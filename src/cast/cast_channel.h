#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ap::cast {

constexpr const char* kHeartbeatNamespace  = "urn:x-cast:com.google.cast.tp.heartbeat";
constexpr const char* kConnectionNamespace = "urn:x-cast:com.google.cast.tp.connection";
constexpr const char* kReceiverNamespace   = "urn:x-cast:com.google.cast.receiver";
constexpr const char* kMediaNamespace      = "urn:x-cast:com.google.cast.media";
constexpr const char* kDeviceAuthNamespace = "urn:x-cast:com.google.cast.tp.deviceauth";

struct CastMessage {
    int protocol_version = 0;
    std::string source_id;
    std::string destination_id;
    std::string ns; // namespace
    int payload_type = 0; // 0 = STRING, 1 = BINARY
    std::string payload_utf8;
    std::vector<uint8_t> payload_binary;
};

// Encodes a CastMessage into length-prefixed bytes (4-byte big-endian size + protobuf data)
std::vector<uint8_t> encode_framed_message(const CastMessage& msg);

// Decodes a raw protobuf buffer into a CastMessage
bool decode_cast_message(const uint8_t* data, size_t len, CastMessage& out);

} // namespace ap::cast
