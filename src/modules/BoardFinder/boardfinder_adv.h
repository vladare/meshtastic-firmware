#pragma once

#include <cstddef>
#include <cstdint>

namespace boardfinder
{

// Meshtastic mesh service UUID bytes as placed in Rider SoftDevice advertising
// (same reverse-order array as MESH_SERVICE_UUID_16).
extern const uint8_t kMeshServiceUuid[16];

constexpr uint8_t AD_TYPE_FLAGS = 0x01;
constexpr uint8_t AD_TYPE_SERVICE_DATA_128BIT = 0x21;

struct RiderAdvMatch {
    uint32_t node_id = 0;
    bool matched = false;
};

// Parse BLE advertising payload for Service Data (128-bit UUID) carrying a LE Node ID.
// Returns true if a well-formed Meshtastic Board Finder Rider advertisement is found.
bool parseRiderServiceData(const uint8_t *adv, size_t advLen, RiderAdvMatch &out);

// True if ads at t1 belong to the same 3 s burst as a prior observation at t0 (monotonic ms).
bool sameBurst(uint32_t firstSeenMs, uint32_t nowMs, uint32_t groupWindowMs = 4000);

} // namespace boardfinder
