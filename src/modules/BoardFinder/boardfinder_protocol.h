#pragma once

#include <cstddef>
#include <cstdint>

// Board Finder PRIVATE_APP command protocol (firmware + Android contract).
// Wire encoding is standard protobuf binary with the field numbers below.
// Official Meshtastic Config/ModuleConfig/AdminMessage protos are not modified.
//
// FROZEN: protocol_version is always 1. Additive commands/fields may be added
// with new unused numbers without changing protocol_version. See
// protocol/boardfinder.proto and docs/PROTOCOL_FREEZE.md.

namespace boardfinder
{

constexpr uint32_t PROTOCOL_VERSION = 1;
constexpr size_t MAX_MESSAGE_BYTES = 192;

enum class Command : uint8_t {
    UNSPECIFIED = 0,
    GET_CONFIG = 1,
    SET_CONFIG = 2,
    CONFIG_RESPONSE = 3,
    ERROR_RESPONSE = 4,
    SET_DEVICE_ROLE = 5,
    SET_RIDER_NODE_ID = 6,
    ARM_BOARD = 7,
    DISARM_BOARD = 8,
    GET_BOARD_STATUS = 9,
    BOARD_STATUS_RESPONSE = 10,
    IDENTIFY_DEVICE = 11,
    IDENTIFY_RESPONSE = 12,
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
    INVALID_ROLE = 6,
    NOT_BOARD = 7,
    RIDER_UNASSIGNED = 8,
    INVALID_STATE = 9,
    INVALID_CONFIG = 10,
};

enum class DeviceRole : uint8_t {
    NORMAL = 0,
    RIDER = 1,
    BOARD = 2,
};

enum class BoardState : uint8_t {
    DISARMED = 0,
    DISCOVERING = 1,
    SYNCHRONIZING = 2,
    MONITORING = 3,
    SEPARATED = 4,
    ERROR = 5,
};

enum class GpsReportStatus : uint8_t {
    NONE = 0,
    OK = 1,
    UNAVAILABLE = 2,
    DEFERRED = 3,
    FAILED = 4,
};

enum class ReportingPhase : uint8_t {
    INACTIVE = 0,
    NORMAL = 1,
    ECONOMY = 2,
};

struct Message {
    uint32_t protocol_version = 0;
    uint32_t request_id = 0;
    Command command = Command::UNSPECIFIED;

    bool ble_burst_enabled = false;
    bool has_ble_burst_enabled = false;

    Status status = Status::OK;
    ErrorCode error_code = ErrorCode::NONE;

    DeviceRole device_role = DeviceRole::NORMAL;
    bool has_device_role = false;

    uint32_t rider_node_id = 0;
    bool has_rider_node_id = false;

    bool board_armed = false;
    bool has_board_armed = false;

    BoardState board_state = BoardState::DISARMED;
    bool has_board_state = false;

    bool synchronized = false;
    bool has_synchronized = false;

    uint32_t consecutive_missed_bursts = 0;
    bool has_consecutive_missed_bursts = false;

    int32_t last_rider_rssi = 0;
    bool has_last_rider_rssi = false;

    uint32_t last_rider_seen_age_seconds = 0;
    bool has_last_rider_seen_age_seconds = false;

    bool gps_tracking_active = false;
    bool has_gps_tracking_active = false;

    GpsReportStatus last_gps_report_status = GpsReportStatus::NONE;
    bool has_last_gps_report_status = false;

    // v3 Economy configuration / status
    bool economy_enabled = true;
    bool has_economy_enabled = false;

    uint32_t economy_after_seconds = 0;
    bool has_economy_after_seconds = false;

    uint32_t normal_gps_interval_seconds = 0;
    bool has_normal_gps_interval_seconds = false;

    uint32_t economy_gps_interval_seconds = 0;
    bool has_economy_gps_interval_seconds = false;

    ReportingPhase reporting_phase = ReportingPhase::INACTIVE;
    bool has_reporting_phase = false;

    uint32_t separation_elapsed_seconds = 0;
    bool has_separation_elapsed_seconds = false;

    uint32_t next_gps_report_due_seconds = 0;
    bool has_next_gps_report_due_seconds = false;
};

// Returns encoded length, or 0 on failure.
size_t encodeMessage(const Message &msg, uint8_t *out, size_t outMax);

// Returns true on successful decode of a well-formed message.
bool decodeMessage(const uint8_t *in, size_t inLen, Message &out);

} // namespace boardfinder
