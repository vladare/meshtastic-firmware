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
constexpr uint8_t TAG_DEVICE_ROLE = 7;
constexpr uint8_t TAG_RIDER_NODE_ID = 8;
constexpr uint8_t TAG_BOARD_ARMED = 9;
constexpr uint8_t TAG_BOARD_STATE = 10;
constexpr uint8_t TAG_SYNCHRONIZED = 11;
constexpr uint8_t TAG_CONSECUTIVE_MISSED = 12;
constexpr uint8_t TAG_LAST_RIDER_RSSI = 13;
constexpr uint8_t TAG_LAST_RIDER_SEEN_AGE = 14;
constexpr uint8_t TAG_GPS_TRACKING_ACTIVE = 15;
constexpr uint8_t TAG_LAST_GPS_REPORT_STATUS = 16;

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
    if (!writeVarint(pp, end, static_cast<uint32_t>(fieldNumber) << 3))
        return false;
    return writeVarint(pp, end, v);
}

uint32_t zigzagEncode(int32_t v)
{
    return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
}

int32_t zigzagDecode(uint32_t v)
{
    return static_cast<int32_t>((v >> 1) ^ (~(v & 1) + 1));
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

bool wantsBleBurstField(const Message &msg)
{
    return msg.has_ble_burst_enabled || msg.command == Command::CONFIG_RESPONSE || msg.command == Command::SET_CONFIG ||
           msg.command == Command::BOARD_STATUS_RESPONSE;
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
    if (wantsBleBurstField(msg)) {
        if (!writeKeyVarint(&p, end, TAG_BLE_BURST_ENABLED, msg.ble_burst_enabled ? 1u : 0u))
            return 0;
    }
    if (!writeKeyVarint(&p, end, TAG_STATUS, static_cast<uint32_t>(msg.status)))
        return 0;
    if (!writeKeyVarint(&p, end, TAG_ERROR_CODE, static_cast<uint32_t>(msg.error_code)))
        return 0;

    if (msg.has_device_role || msg.command == Command::CONFIG_RESPONSE || msg.command == Command::BOARD_STATUS_RESPONSE ||
        msg.command == Command::SET_DEVICE_ROLE) {
        if (!writeKeyVarint(&p, end, TAG_DEVICE_ROLE, static_cast<uint32_t>(msg.device_role)))
            return 0;
    }
    if (msg.has_rider_node_id || msg.command == Command::CONFIG_RESPONSE || msg.command == Command::BOARD_STATUS_RESPONSE ||
        msg.command == Command::SET_RIDER_NODE_ID) {
        if (!writeKeyVarint(&p, end, TAG_RIDER_NODE_ID, msg.rider_node_id))
            return 0;
    }
    if (msg.has_board_armed || msg.command == Command::BOARD_STATUS_RESPONSE) {
        if (!writeKeyVarint(&p, end, TAG_BOARD_ARMED, msg.board_armed ? 1u : 0u))
            return 0;
    }
    if (msg.has_board_state || msg.command == Command::BOARD_STATUS_RESPONSE) {
        if (!writeKeyVarint(&p, end, TAG_BOARD_STATE, static_cast<uint32_t>(msg.board_state)))
            return 0;
    }
    if (msg.has_synchronized || msg.command == Command::BOARD_STATUS_RESPONSE) {
        if (!writeKeyVarint(&p, end, TAG_SYNCHRONIZED, msg.synchronized ? 1u : 0u))
            return 0;
    }
    if (msg.has_consecutive_missed_bursts || msg.command == Command::BOARD_STATUS_RESPONSE) {
        if (!writeKeyVarint(&p, end, TAG_CONSECUTIVE_MISSED, msg.consecutive_missed_bursts))
            return 0;
    }
    if (msg.has_last_rider_rssi) {
        if (!writeKeyVarint(&p, end, TAG_LAST_RIDER_RSSI, zigzagEncode(msg.last_rider_rssi)))
            return 0;
    }
    if (msg.has_last_rider_seen_age_seconds) {
        if (!writeKeyVarint(&p, end, TAG_LAST_RIDER_SEEN_AGE, msg.last_rider_seen_age_seconds))
            return 0;
    }
    if (msg.has_gps_tracking_active || msg.command == Command::BOARD_STATUS_RESPONSE) {
        if (!writeKeyVarint(&p, end, TAG_GPS_TRACKING_ACTIVE, msg.gps_tracking_active ? 1u : 0u))
            return 0;
    }
    if (msg.has_last_gps_report_status || msg.command == Command::BOARD_STATUS_RESPONSE) {
        if (!writeKeyVarint(&p, end, TAG_LAST_GPS_REPORT_STATUS, static_cast<uint32_t>(msg.last_gps_report_status)))
            return 0;
    }

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
        case TAG_DEVICE_ROLE:
            out.device_role = static_cast<DeviceRole>(value);
            out.has_device_role = true;
            break;
        case TAG_RIDER_NODE_ID:
            out.rider_node_id = value;
            out.has_rider_node_id = true;
            break;
        case TAG_BOARD_ARMED:
            out.board_armed = value != 0;
            out.has_board_armed = true;
            break;
        case TAG_BOARD_STATE:
            out.board_state = static_cast<BoardState>(value);
            out.has_board_state = true;
            break;
        case TAG_SYNCHRONIZED:
            out.synchronized = value != 0;
            out.has_synchronized = true;
            break;
        case TAG_CONSECUTIVE_MISSED:
            out.consecutive_missed_bursts = value;
            out.has_consecutive_missed_bursts = true;
            break;
        case TAG_LAST_RIDER_RSSI:
            out.last_rider_rssi = zigzagDecode(value);
            out.has_last_rider_rssi = true;
            break;
        case TAG_LAST_RIDER_SEEN_AGE:
            out.last_rider_seen_age_seconds = value;
            out.has_last_rider_seen_age_seconds = true;
            break;
        case TAG_GPS_TRACKING_ACTIVE:
            out.gps_tracking_active = value != 0;
            out.has_gps_tracking_active = true;
            break;
        case TAG_LAST_GPS_REPORT_STATUS:
            out.last_gps_report_status = static_cast<GpsReportStatus>(value);
            out.has_last_gps_report_status = true;
            break;
        default:
            break;
        }
    }

    return sawCommand;
}

} // namespace boardfinder
