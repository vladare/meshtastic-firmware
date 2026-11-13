#include "boardfinder_adv.h"
#include <cstring>

namespace boardfinder
{

// Must match BluetoothCommon.cpp MESH_SERVICE_UUID_16 (SoftDevice reverse order).
const uint8_t kMeshServiceUuid[16] = {0xfd, 0xea, 0x73, 0xe2, 0xca, 0x5d, 0xa8, 0x9f,
                                      0x1f, 0x46, 0xa8, 0x15, 0x18, 0xb2, 0xa1, 0x6b};

bool parseRiderServiceData(const uint8_t *adv, size_t advLen, RiderAdvMatch &out)
{
    out = RiderAdvMatch{};
    if (adv == nullptr || advLen < 3)
        return false;

    size_t i = 0;
    while (i < advLen) {
        const uint8_t len = adv[i];
        if (len == 0)
            break;
        if (i + 1 + len > advLen)
            return false;
        const uint8_t type = adv[i + 1];
        const uint8_t *data = &adv[i + 2];
        const uint8_t dataLen = static_cast<uint8_t>(len - 1);

        if (type == AD_TYPE_SERVICE_DATA_128BIT) {
            // UUID (16) + node id (4)
            if (dataLen < 20)
                return false;
            if (memcmp(data, kMeshServiceUuid, 16) != 0) {
                i += static_cast<size_t>(len) + 1;
                continue;
            }
            out.node_id = static_cast<uint32_t>(data[16]) | (static_cast<uint32_t>(data[17]) << 8) |
                          (static_cast<uint32_t>(data[18]) << 16) | (static_cast<uint32_t>(data[19]) << 24);
            out.matched = true;
            return true;
        }

        i += static_cast<size_t>(len) + 1;
    }
    return false;
}

bool sameBurst(uint32_t firstSeenMs, uint32_t nowMs, uint32_t groupWindowMs)
{
    return (nowMs - firstSeenMs) <= groupWindowMs;
}

} // namespace boardfinder
