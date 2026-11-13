#pragma once

#include <cstddef>
#include <cstdint>

// Board Finder PRIVATE_APP command protocol (firmware + Android contract).
// Wire encoding is standard protobuf binary with the field numbers below.
// Official Meshtastic Config/ModuleConfig/AdminMessage protos are not modified.

namespace boardfinder
{

constexpr uint32_t PROTOCOL_VERSION = 1;
constexpr size_t MAX_MESSAGE_BYTES = 64;

enum class Command : uint8_t {
    UNSPECIFIED = 0,
    GET_CONFIG = 1,
    SET_CONFIG = 2,
    CONFIG_RESPONSE = 3,
    ERROR_RESPONSE = 4,
};

enum class Status : uint8_t {
    OK = 0,
    ERROR = 1,
};

enum class ErrorCode : uint8_t {
    NONE = 0,
    UNSUPPORTED_VERSION = 1,
    UNSUPPORTED_COMMAND = 2,
    MALFORMED = 3,
    NOT_LOCAL = 4,
    INTERNAL = 5,
};

struct Message {
    uint32_t protocol_version = 0;
    uint32_t request_id = 0;
    Command command = Command::UNSPECIFIED;
    bool ble_burst_enabled = false;
    bool has_ble_burst_enabled = false;
    Status status = Status::OK;
    ErrorCode error_code = ErrorCode::NONE;
};

// Returns encoded length, or 0 on failure.
size_t encodeMessage(const Message &msg, uint8_t *out, size_t outMax);

// Returns true on successful decode of a well-formed message.
bool decodeMessage(const uint8_t *in, size_t inLen, Message &out);

} // namespace boardfinder
