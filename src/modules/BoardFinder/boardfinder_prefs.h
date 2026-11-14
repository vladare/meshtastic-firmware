#pragma once

#include "boardfinder_economy.h"
#include "boardfinder_protocol.h"
#include <cstddef>
#include <cstdint>

// Internal flash record format for Board Finder preferences.
// PrefsRecord version is independent of Board Finder wire protocol_version (always 1).

namespace boardfinder
{

constexpr uint32_t PREFS_MAGIC = 0xBF100001u;
constexpr uint32_t PREFS_VERSION_V1 = 1;
constexpr uint32_t PREFS_VERSION_V2 = 2;
constexpr uint32_t PREFS_VERSION_V3 = 3;
constexpr uint32_t PREFS_VERSION = 4; // PrefsRecordV4 (not protocol v4)

struct PersistentPrefs {
    bool ble_burst_enabled = false;
    DeviceRole device_role = DeviceRole::NORMAL;
    uint32_t rider_node_id = 0;
    EconomySettings economy = defaultEconomySettings();
    bool sos_transmission_enabled = false; // default OFF
};

#pragma pack(push, 1)
struct PrefsRecordV1 {
    uint32_t magic;
    uint32_t version;
    uint8_t ble_burst_enabled;
    uint8_t reserved[3];
};

struct PrefsRecordV2 {
    uint32_t magic;
    uint32_t version;
    uint8_t ble_burst_enabled;
    uint8_t device_role;
    uint8_t board_armed_deprecated;
    uint8_t reserved;
    uint32_t rider_node_id;
};

struct PrefsRecordV3 {
    uint32_t magic;
    uint32_t version;
    uint8_t ble_burst_enabled;
    uint8_t device_role;
    uint8_t board_armed_deprecated;
    uint8_t economy_enabled;
    uint32_t rider_node_id;
    uint32_t economy_after_seconds;
    uint32_t normal_gps_interval_seconds;
    uint32_t economy_gps_interval_seconds;
};

struct PrefsRecordV4 {
    uint32_t magic;
    uint32_t version;
    uint8_t ble_burst_enabled;
    uint8_t device_role;
    uint8_t board_armed_deprecated;
    uint8_t economy_enabled;
    uint32_t rider_node_id;
    uint32_t economy_after_seconds;
    uint32_t normal_gps_interval_seconds;
    uint32_t economy_gps_interval_seconds;
    uint8_t sos_transmission_enabled;
    uint8_t reserved[3];
};
#pragma pack(pop)

// Parse a raw flash blob (any supported record size). Defaults sos_transmission_enabled=false on V1–V3.
// Returns false if the blob is invalid; out is left at safe defaults.
bool parsePersistentPrefs(const uint8_t *data, size_t len, PersistentPrefs &out);

// Encode current prefs as PrefsRecordV4. Returns encoded length or 0 on failure.
size_t encodePersistentPrefs(const PersistentPrefs &in, uint8_t *out, size_t outMax);

/** TX gate helper: module must be present and flag enabled. Default-safe when either is false. */
inline bool isSosTransmissionAllowed(bool modulePresent, bool sosTransmissionEnabled)
{
    return modulePresent && sosTransmissionEnabled;
}

} // namespace boardfinder
