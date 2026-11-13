#include "boardfinder_protocol.h"

namespace boardfinder
{
namespace
{

constexpr uint8_t TAG_PROTOCOL_VERSION = 1;
constexpr uint8_t TAG_REQUEST_ID = 2;
constexpr uint8_t TAG_COMMAND = 3;
constexpr uint8_t TAG_BLE_BURST_ENABLED = 4;
constexpr uint8_t TAG_STATUS = 5;
constexpr uint8_t TAG_ERROR_CODE = 6;

bool writeByte(uint8_t **pp, uint8_t *end, uint8_t v)
{
    if (*pp >= end)
        return false;
    *(*pp)++ = v;
    return true;
}

bool writeVarint(uint8_t **pp, uint8_t *end, uint32_t v)
{
    while (v >= 0x80) {
        if (!writeByte(pp, end, static_cast<uint8_t>(v | 0x80)))
            return false;
        v >>= 7;
    }
    return writeByte(pp, end, static_cast<uint8_t>(v));
}

bool writeKeyVarint(uint8_t **pp, uint8_t *end, uint8_t fieldNumber, uint32_t v)
{
    // wire type 0 = varint
    if (!writeVarint(pp, end, static_cast<uint32_t>(fieldNumber) << 3))
        return false;
    return writeVarint(pp, end, v);
}

bool readVarint(const uint8_t **pp, const uint8_t *end, uint32_t &out)
{
    uint32_t result = 0;
    uint32_t shift = 0;
    while (*pp < end) {
        uint8_t b = *(*pp)++;
        result |= static_cast<uint32_t>(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            out = result;
            return true;
        }
        shift += 7;
        if (shift > 28)
            return false;
    }
    return false;
}

} // namespace

size_t encodeMessage(const Message &msg, uint8_t *out, size_t outMax)
{
    if (out == nullptr || outMax == 0)
        return 0;

    uint8_t *p = out;
    uint8_t *end = out + outMax;

    if (!writeKeyVarint(&p, end, TAG_PROTOCOL_VERSION, msg.protocol_version))
        return 0;
    if (!writeKeyVarint(&p, end, TAG_REQUEST_ID, msg.request_id))
        return 0;
    if (!writeKeyVarint(&p, end, TAG_COMMAND, static_cast<uint32_t>(msg.command)))
        return 0;
    if (msg.has_ble_burst_enabled || msg.command == Command::CONFIG_RESPONSE || msg.command == Command::SET_CONFIG) {
        if (!writeKeyVarint(&p, end, TAG_BLE_BURST_ENABLED, msg.ble_burst_enabled ? 1u : 0u))
            return 0;
    }
    if (!writeKeyVarint(&p, end, TAG_STATUS, static_cast<uint32_t>(msg.status)))
        return 0;
    if (!writeKeyVarint(&p, end, TAG_ERROR_CODE, static_cast<uint32_t>(msg.error_code)))
        return 0;

    return static_cast<size_t>(p - out);
}

bool decodeMessage(const uint8_t *in, size_t inLen, Message &out)
{
    out = Message{};
    if (in == nullptr || inLen == 0)
        return false;

    const uint8_t *p = in;
    const uint8_t *end = in + inLen;
    bool sawCommand = false;

    while (p < end) {
        uint32_t key = 0;
        if (!readVarint(&p, end, key))
            return false;
        uint32_t field = key >> 3;
        uint32_t wire = key & 0x7;
        if (wire != 0) {
            // Skip length-delimited or fixed fields if ever present.
            if (wire == 2) {
                uint32_t len = 0;
                if (!readVarint(&p, end, len) || p + len > end)
                    return false;
                p += len;
                continue;
            }
            if (wire == 5) {
                if (p + 4 > end)
                    return false;
                p += 4;
                continue;
            }
            if (wire == 1) {
                if (p + 8 > end)
                    return false;
                p += 8;
                continue;
            }
            return false;
        }

        uint32_t value = 0;
        if (!readVarint(&p, end, value))
            return false;

        switch (field) {
        case TAG_PROTOCOL_VERSION:
            out.protocol_version = value;
            break;
        case TAG_REQUEST_ID:
            out.request_id = value;
            break;
        case TAG_COMMAND:
            out.command = static_cast<Command>(value);
            sawCommand = true;
            break;
        case TAG_BLE_BURST_ENABLED:
            out.ble_burst_enabled = value != 0;
            out.has_ble_burst_enabled = true;
            break;
        case TAG_STATUS:
            out.status = static_cast<Status>(value);
            break;
        case TAG_ERROR_CODE:
            out.error_code = static_cast<ErrorCode>(value);
            break;
        default:
            break;
        }
    }

    return sawCommand;
}

} // namespace boardfinder
