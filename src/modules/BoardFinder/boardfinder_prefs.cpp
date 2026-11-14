#include "boardfinder_prefs.h"
#include <cstring>

namespace boardfinder
{
namespace
{

bool roleValid(uint32_t v)
{
    return v <= static_cast<uint32_t>(DeviceRole::BOARD);
}

void applyDefaultEconomy(EconomySettings &eco)
{
    eco = defaultEconomySettings();
}

} // namespace

bool parsePersistentPrefs(const uint8_t *data, size_t len, PersistentPrefs &out)
{
    out = PersistentPrefs{};
    applyDefaultEconomy(out.economy);
    out.sos_transmission_enabled = false;

    if (data == nullptr || len == 0)
        return true; // missing file → defaults

    if (len == sizeof(PrefsRecordV1)) {
        PrefsRecordV1 v1{};
        memcpy(&v1, data, sizeof(v1));
        if (v1.magic != PREFS_MAGIC || v1.version != PREFS_VERSION_V1)
            return false;
        out.ble_burst_enabled = v1.ble_burst_enabled != 0;
        out.device_role = DeviceRole::NORMAL;
        out.rider_node_id = 0;
        applyDefaultEconomy(out.economy);
        out.sos_transmission_enabled = false;
        return true;
    }

    if (len == sizeof(PrefsRecordV2)) {
        PrefsRecordV2 v2{};
        memcpy(&v2, data, sizeof(v2));
        if (v2.magic != PREFS_MAGIC || v2.version == 0 || v2.version > PREFS_VERSION_V2 || !roleValid(v2.device_role))
            return false;
        out.ble_burst_enabled = v2.ble_burst_enabled != 0;
        out.device_role = static_cast<DeviceRole>(v2.device_role);
        out.rider_node_id = v2.rider_node_id;
        applyDefaultEconomy(out.economy);
        out.sos_transmission_enabled = false;
        (void)v2.board_armed_deprecated;
        return true;
    }

    if (len == sizeof(PrefsRecordV3)) {
        PrefsRecordV3 v3{};
        memcpy(&v3, data, sizeof(v3));
        if (v3.magic != PREFS_MAGIC || v3.version == 0 || v3.version > PREFS_VERSION_V3 || !roleValid(v3.device_role))
            return false;
        out.ble_burst_enabled = v3.ble_burst_enabled != 0;
        out.device_role = static_cast<DeviceRole>(v3.device_role);
        out.rider_node_id = v3.rider_node_id;
        (void)v3.board_armed_deprecated;

        EconomySettings eco{};
        eco.economy_enabled = v3.economy_enabled != 0;
        eco.economy_after_seconds = v3.economy_after_seconds;
        eco.normal_gps_interval_seconds = v3.normal_gps_interval_seconds;
        eco.economy_gps_interval_seconds = v3.economy_gps_interval_seconds;
        if (!validateEconomySettings(eco))
            applyDefaultEconomy(eco);
        out.economy = eco;
        out.sos_transmission_enabled = false; // V3→V4 default OFF
        return true;
    }

    if (len != sizeof(PrefsRecordV4))
        return false;

    PrefsRecordV4 rec{};
    memcpy(&rec, data, sizeof(rec));
    if (rec.magic != PREFS_MAGIC || rec.version == 0 || rec.version > PREFS_VERSION || !roleValid(rec.device_role))
        return false;

    out.ble_burst_enabled = rec.ble_burst_enabled != 0;
    out.device_role = static_cast<DeviceRole>(rec.device_role);
    out.rider_node_id = rec.rider_node_id;
    (void)rec.board_armed_deprecated;

    EconomySettings eco{};
    eco.economy_enabled = rec.economy_enabled != 0;
    eco.economy_after_seconds = rec.economy_after_seconds;
    eco.normal_gps_interval_seconds = rec.normal_gps_interval_seconds;
    eco.economy_gps_interval_seconds = rec.economy_gps_interval_seconds;
    if (!validateEconomySettings(eco))
        applyDefaultEconomy(eco);
    out.economy = eco;
    out.sos_transmission_enabled = rec.sos_transmission_enabled != 0;
    return true;
}

size_t encodePersistentPrefs(const PersistentPrefs &in, uint8_t *out, size_t outMax)
{
    if (out == nullptr || outMax < sizeof(PrefsRecordV4))
        return 0;

    PrefsRecordV4 rec{};
    rec.magic = PREFS_MAGIC;
    rec.version = PREFS_VERSION;
    rec.ble_burst_enabled = in.ble_burst_enabled ? 1 : 0;
    rec.device_role = static_cast<uint8_t>(in.device_role);
    rec.board_armed_deprecated = 0;
    rec.economy_enabled = in.economy.economy_enabled ? 1 : 0;
    rec.rider_node_id = in.rider_node_id;
    rec.economy_after_seconds = in.economy.economy_after_seconds;
    rec.normal_gps_interval_seconds = in.economy.normal_gps_interval_seconds;
    rec.economy_gps_interval_seconds = in.economy.economy_gps_interval_seconds;
    rec.sos_transmission_enabled = in.sos_transmission_enabled ? 1 : 0;
    rec.reserved[0] = rec.reserved[1] = rec.reserved[2] = 0;

    memcpy(out, &rec, sizeof(rec));
    return sizeof(rec);
}

} // namespace boardfinder
