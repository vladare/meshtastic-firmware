#if defined(TRACKER_T1000_E)

#include "BoardFinderModule.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "boardfinder_identify.h"
#include "configuration.h"
#include "main.h"
#include "power/PowerHAL.h"
#include <cstring>

BoardFinderModule *boardFinderModule;

namespace
{
constexpr const char *kPrefsPath = "/prefs/boardfinder.proto";
constexpr uint32_t kPrefsMagic = 0xBF100001u;
constexpr uint32_t kPrefsVersionV1 = 1;
constexpr uint32_t kPrefsVersionV2 = 2;
constexpr uint32_t kPrefsVersion = 3;

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
    uint8_t board_armed_deprecated; // ignored at load; always written 0
    uint8_t economy_enabled;
    uint32_t rider_node_id;
    uint32_t economy_after_seconds;
    uint32_t normal_gps_interval_seconds;
    uint32_t economy_gps_interval_seconds;
};

bool roleValid(uint32_t v)
{
    return v <= static_cast<uint32_t>(boardfinder::DeviceRole::BOARD);
}

void applyDefaultEconomy(boardfinder::EconomySettings &eco)
{
    eco = boardfinder::defaultEconomySettings();
}

} // namespace

concurrency::Lock &BoardFinderModule::prefsLock()
{
    static concurrency::Lock lock;
    return lock;
}

BoardFinderModule::BoardFinderModule() : SinglePortModule("BoardFinder", meshtastic_PortNum_PRIVATE_APP)
{
    loadPrefs(prefs);
    runtimeArmed = false; // every boot starts DISARMED; ignore any legacy flash ARM flag
    boardFinderEngine = new BoardFinderEngine();
    applyRuntime(false);
    LOG_INFO("BoardFinder ready role=%u burst=%d armed=0 rider=0x%08x eco=%d/%u/%u/%u",
             static_cast<unsigned>(prefs.device_role), prefs.ble_burst_enabled ? 1 : 0, prefs.rider_node_id,
             prefs.economy.economy_enabled ? 1 : 0, prefs.economy.economy_after_seconds,
             prefs.economy.normal_gps_interval_seconds, prefs.economy.economy_gps_interval_seconds);
}

bool BoardFinderModule::isLocalPhoneConfigRequest(const meshtastic_MeshPacket &mp)
{
    if (mp.from != 0)
        return false;
    if (isBroadcast(mp.to) || !isToUs(&mp))
        return false;
    return true;
}

bool BoardFinderModule::loadPrefs(Prefs &out)
{
    out = Prefs{};
    applyDefaultEconomy(out.economy);
    concurrency::LockGuard guard(&prefsLock());
#ifdef FSCom
    concurrency::LockGuard spi(spiLock);
    if (!FSCom.exists(kPrefsPath))
        return true;

    auto f = FSCom.open(kPrefsPath, FILE_O_READ);
    if (!f)
        return false;

    uint8_t buf[sizeof(PrefsRecordV3)] = {};
    const size_t n = f.read(buf, sizeof(buf));
    f.close();

    if (n == sizeof(PrefsRecordV1)) {
        PrefsRecordV1 v1{};
        memcpy(&v1, buf, sizeof(v1));
        if (v1.magic != kPrefsMagic || v1.version != kPrefsVersionV1) {
            LOG_WARN("BoardFinder prefs v1 invalid; defaults");
            return false;
        }
        out.ble_burst_enabled = v1.ble_burst_enabled != 0;
        out.device_role = boardfinder::DeviceRole::NORMAL;
        out.rider_node_id = 0;
        applyDefaultEconomy(out.economy);
        LOG_INFO("BoardFinder migrated prefs v1->v3 burst=%d", out.ble_burst_enabled ? 1 : 0);
        return true;
    }

    if (n == sizeof(PrefsRecordV2)) {
        PrefsRecordV2 v2{};
        memcpy(&v2, buf, sizeof(v2));
        if (v2.magic != kPrefsMagic || v2.version == 0 || v2.version > kPrefsVersionV2 || !roleValid(v2.device_role)) {
            LOG_WARN("BoardFinder prefs v2 invalid; defaults");
            return false;
        }
        out.ble_burst_enabled = v2.ble_burst_enabled != 0;
        out.device_role = static_cast<boardfinder::DeviceRole>(v2.device_role);
        out.rider_node_id = v2.rider_node_id;
        applyDefaultEconomy(out.economy);
        (void)v2.board_armed_deprecated;
        LOG_INFO("BoardFinder migrated prefs v2->v3 role=%u rider=0x%08x", static_cast<unsigned>(out.device_role),
                 out.rider_node_id);
        return true;
    }

    if (n != sizeof(PrefsRecordV3)) {
        LOG_WARN("BoardFinder prefs size mismatch; defaults");
        return false;
    }

    PrefsRecordV3 rec{};
    memcpy(&rec, buf, sizeof(rec));
    if (rec.magic != kPrefsMagic || rec.version == 0 || rec.version > kPrefsVersion || !roleValid(rec.device_role)) {
        LOG_WARN("BoardFinder prefs invalid; defaults");
        return false;
    }

    out.ble_burst_enabled = rec.ble_burst_enabled != 0;
    out.device_role = static_cast<boardfinder::DeviceRole>(rec.device_role);
    out.rider_node_id = rec.rider_node_id;
    (void)rec.board_armed_deprecated;

    boardfinder::EconomySettings eco{};
    eco.economy_enabled = rec.economy_enabled != 0;
    eco.economy_after_seconds = rec.economy_after_seconds;
    eco.normal_gps_interval_seconds = rec.normal_gps_interval_seconds;
    eco.economy_gps_interval_seconds = rec.economy_gps_interval_seconds;
    if (!boardfinder::validateEconomySettings(eco)) {
        LOG_WARN("BoardFinder economy prefs invalid; using defaults");
        applyDefaultEconomy(eco);
    }
    out.economy = eco;
    return true;
#else
    (void)out;
    return true;
#endif
}

bool BoardFinderModule::savePrefs(const Prefs &in)
{
    concurrency::LockGuard guard(&prefsLock());
#ifdef FSCom
    if (!powerHAL_isPowerLevelSafe()) {
        LOG_ERROR("BoardFinder prefs save blocked: unsafe power");
        return false;
    }

    PrefsRecordV3 rec{};
    rec.magic = kPrefsMagic;
    rec.version = kPrefsVersion;
    rec.ble_burst_enabled = in.ble_burst_enabled ? 1 : 0;
    rec.device_role = static_cast<uint8_t>(in.device_role);
    rec.board_armed_deprecated = 0;
    rec.economy_enabled = in.economy.economy_enabled ? 1 : 0;
    rec.rider_node_id = in.rider_node_id;
    rec.economy_after_seconds = in.economy.economy_after_seconds;
    rec.normal_gps_interval_seconds = in.economy.normal_gps_interval_seconds;
    rec.economy_gps_interval_seconds = in.economy.economy_gps_interval_seconds;

    SafeFile f(kPrefsPath, true);
    size_t written = f.write(reinterpret_cast<const uint8_t *>(&rec), sizeof(rec));
    bool ok = f.close() && written == sizeof(rec);
    if (!ok)
        LOG_ERROR("BoardFinder prefs save failed");
    return ok;
#else
    (void)in;
    return false;
#endif
}

bool BoardFinderModule::persistIfChanged(const Prefs &next)
{
    if (next.ble_burst_enabled == prefs.ble_burst_enabled && next.device_role == prefs.device_role &&
        next.rider_node_id == prefs.rider_node_id && next.economy.economy_enabled == prefs.economy.economy_enabled &&
        next.economy.economy_after_seconds == prefs.economy.economy_after_seconds &&
        next.economy.normal_gps_interval_seconds == prefs.economy.normal_gps_interval_seconds &&
        next.economy.economy_gps_interval_seconds == prefs.economy.economy_gps_interval_seconds) {
        return true;
    }
    if (!savePrefs(next))
        return false;
    prefs = next;
    return true;
}

void BoardFinderModule::applyRuntime(bool restartIfArmed)
{
    if (!boardFinderEngine)
        return;
    BoardFinderEngine::Config cfg{};
    cfg.role = prefs.device_role;
    cfg.rider_node_id = prefs.rider_node_id;
    cfg.board_armed = runtimeArmed;
    cfg.ble_burst_enabled = prefs.ble_burst_enabled;
    cfg.economy = prefs.economy;
    boardFinderEngine->applyConfig(cfg, restartIfArmed);
}

void BoardFinderModule::fillEconomyFields(boardfinder::Message &msg) const
{
    msg.economy_enabled = prefs.economy.economy_enabled;
    msg.has_economy_enabled = true;
    msg.economy_after_seconds = prefs.economy.economy_after_seconds;
    msg.has_economy_after_seconds = true;
    msg.normal_gps_interval_seconds = prefs.economy.normal_gps_interval_seconds;
    msg.has_normal_gps_interval_seconds = true;
    msg.economy_gps_interval_seconds = prefs.economy.economy_gps_interval_seconds;
    msg.has_economy_gps_interval_seconds = true;
}

void BoardFinderModule::replyError(uint32_t requestId, boardfinder::ErrorCode code)
{
    pendingReply = boardfinder::Message{};
    pendingReply.protocol_version = boardfinder::PROTOCOL_VERSION;
    pendingReply.request_id = requestId;
    pendingReply.command = boardfinder::Command::ERROR_RESPONSE;
    pendingReply.status = boardfinder::Status::ERROR;
    pendingReply.error_code = code;
    hasPendingReply = true;
}

void BoardFinderModule::replyConfig(uint32_t requestId)
{
    pendingReply = boardfinder::Message{};
    pendingReply.protocol_version = boardfinder::PROTOCOL_VERSION;
    pendingReply.request_id = requestId;
    pendingReply.command = boardfinder::Command::CONFIG_RESPONSE;
    pendingReply.ble_burst_enabled = prefs.ble_burst_enabled;
    pendingReply.has_ble_burst_enabled = true;
    pendingReply.device_role = prefs.device_role;
    pendingReply.has_device_role = true;
    pendingReply.rider_node_id = prefs.rider_node_id;
    pendingReply.has_rider_node_id = true;
    pendingReply.board_armed = runtimeArmed;
    pendingReply.has_board_armed = true;
    fillEconomyFields(pendingReply);
    pendingReply.status = boardfinder::Status::OK;
    pendingReply.error_code = boardfinder::ErrorCode::NONE;
    hasPendingReply = true;
}

void BoardFinderModule::replyBoardStatus(uint32_t requestId)
{
    auto snap = boardFinderEngine ? boardFinderEngine->status() : BoardFinderEngine::StatusSnapshot{};
    pendingReply = boardfinder::Message{};
    pendingReply.protocol_version = boardfinder::PROTOCOL_VERSION;
    pendingReply.request_id = requestId;
    pendingReply.command = boardfinder::Command::BOARD_STATUS_RESPONSE;
    pendingReply.ble_burst_enabled = prefs.ble_burst_enabled;
    pendingReply.has_ble_burst_enabled = true;
    pendingReply.device_role = prefs.device_role;
    pendingReply.has_device_role = true;
    pendingReply.rider_node_id = prefs.rider_node_id;
    pendingReply.has_rider_node_id = true;
    pendingReply.board_armed = snap.board_armed;
    pendingReply.has_board_armed = true;
    pendingReply.board_state = snap.board_state;
    pendingReply.has_board_state = true;
    pendingReply.synchronized = snap.synchronized;
    pendingReply.has_synchronized = true;
    pendingReply.consecutive_missed_bursts = snap.consecutive_missed_bursts;
    pendingReply.has_consecutive_missed_bursts = true;
    if (snap.has_last_rider_rssi) {
        pendingReply.last_rider_rssi = snap.last_rider_rssi;
        pendingReply.has_last_rider_rssi = true;
    }
    if (snap.has_last_rider_seen_age_seconds) {
        pendingReply.last_rider_seen_age_seconds = snap.last_rider_seen_age_seconds;
        pendingReply.has_last_rider_seen_age_seconds = true;
    }
    pendingReply.gps_tracking_active = snap.gps_tracking_active;
    pendingReply.has_gps_tracking_active = true;
    pendingReply.last_gps_report_status = snap.last_gps_report_status;
    pendingReply.has_last_gps_report_status = true;
    fillEconomyFields(pendingReply);
    pendingReply.reporting_phase = snap.reporting_phase;
    pendingReply.has_reporting_phase = true;
    if (snap.has_separation_elapsed_seconds) {
        pendingReply.separation_elapsed_seconds = snap.separation_elapsed_seconds;
        pendingReply.has_separation_elapsed_seconds = true;
    }
    if (snap.has_next_gps_report_due_seconds) {
        pendingReply.next_gps_report_due_seconds = snap.next_gps_report_due_seconds;
        pendingReply.has_next_gps_report_due_seconds = true;
    }
    pendingReply.status = boardfinder::Status::OK;
    pendingReply.error_code = boardfinder::ErrorCode::NONE;
    hasPendingReply = true;
}

void BoardFinderModule::deliverReply(const meshtastic_MeshPacket &mp)
{
    if (!hasPendingReply || service == nullptr || nodeDB == nullptr)
        return;
    uint8_t buf[boardfinder::MAX_MESSAGE_BYTES];
    size_t len = boardfinder::encodeMessage(pendingReply, buf, sizeof(buf));
    hasPendingReply = false;
    if (len == 0)
        return;
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = nodeDB->getNodeNum();
    p->from = nodeDB->getNodeNum();
    p->hop_limit = 0;
    p->want_ack = false;
    p->decoded.request_id = mp.id;
    p->decoded.payload.size = static_cast<pb_size_t>(len);
    memcpy(p->decoded.payload.bytes, buf, len);
    service->sendToPhone(p);
}

ProcessMessage BoardFinderModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        mp.decoded.portnum != meshtastic_PortNum_PRIVATE_APP) {
        return ProcessMessage::CONTINUE;
    }

    hasPendingReply = false;

    if (!isLocalPhoneConfigRequest(mp)) {
        LOG_WARN("BoardFinder rejected non-local PRIVATE_APP from=0x%x to=0x%x", mp.from, mp.to);
        return ProcessMessage::STOP;
    }

    boardfinder::Message req{};
    if (!boardfinder::decodeMessage(mp.decoded.payload.bytes, mp.decoded.payload.size, req)) {
        replyError(0, boardfinder::ErrorCode::MALFORMED);
        deliverReply(mp);
        return ProcessMessage::STOP;
    }

    if (req.protocol_version != boardfinder::PROTOCOL_VERSION) {
        replyError(req.request_id, boardfinder::ErrorCode::UNSUPPORTED_VERSION);
        deliverReply(mp);
        return ProcessMessage::STOP;
    }

    switch (req.command) {
    case boardfinder::Command::GET_CONFIG:
        replyConfig(req.request_id);
        break;

    case boardfinder::Command::SET_CONFIG: {
        const bool hasEconomy = req.has_economy_enabled || req.has_economy_after_seconds ||
                                req.has_normal_gps_interval_seconds || req.has_economy_gps_interval_seconds;
        if (!req.has_ble_burst_enabled && !req.has_device_role && !req.has_rider_node_id && !hasEconomy) {
            replyError(req.request_id, boardfinder::ErrorCode::MALFORMED);
            break;
        }
        Prefs next = prefs;
        bool disarmRuntime = false;
        if (req.has_ble_burst_enabled)
            next.ble_burst_enabled = req.ble_burst_enabled;
        if (req.has_device_role) {
            if (!roleValid(static_cast<uint32_t>(req.device_role))) {
                replyError(req.request_id, boardfinder::ErrorCode::INVALID_ROLE);
                break;
            }
            next.device_role = req.device_role;
            if (next.device_role != boardfinder::DeviceRole::BOARD)
                disarmRuntime = true;
        }
        if (req.has_rider_node_id) {
            if (runtimeArmed && req.rider_node_id != prefs.rider_node_id)
                disarmRuntime = true;
            next.rider_node_id = req.rider_node_id;
            if (next.rider_node_id == 0)
                disarmRuntime = true;
        }
        if (hasEconomy) {
            if (req.has_economy_enabled)
                next.economy.economy_enabled = req.economy_enabled;
            if (req.has_economy_after_seconds)
                next.economy.economy_after_seconds = req.economy_after_seconds;
            if (req.has_normal_gps_interval_seconds)
                next.economy.normal_gps_interval_seconds = req.normal_gps_interval_seconds;
            if (req.has_economy_gps_interval_seconds)
                next.economy.economy_gps_interval_seconds = req.economy_gps_interval_seconds;
            if (!boardfinder::validateEconomySettings(next.economy)) {
                replyError(req.request_id, boardfinder::ErrorCode::INVALID_CONFIG);
                break;
            }
        }
        if (!persistIfChanged(next)) {
            replyError(req.request_id, boardfinder::ErrorCode::INTERNAL);
            break;
        }
        if (disarmRuntime) {
            runtimeArmed = false;
            if (boardFinderEngine)
                boardFinderEngine->disarm();
        }
        // Economy-only updates must not restart an armed session.
        const bool structural = req.has_device_role || req.has_rider_node_id;
        applyRuntime(structural);
        replyConfig(req.request_id);
        break;
    }

    case boardfinder::Command::SET_DEVICE_ROLE: {
        if (!req.has_device_role || !roleValid(static_cast<uint32_t>(req.device_role))) {
            replyError(req.request_id, boardfinder::ErrorCode::INVALID_ROLE);
            break;
        }
        Prefs next = prefs;
        next.device_role = req.device_role;
        if (!persistIfChanged(next)) {
            replyError(req.request_id, boardfinder::ErrorCode::INTERNAL);
            break;
        }
        if (next.device_role != boardfinder::DeviceRole::BOARD) {
            runtimeArmed = false;
            if (boardFinderEngine)
                boardFinderEngine->disarm();
        }
        applyRuntime(true);
        replyConfig(req.request_id);
        break;
    }

    case boardfinder::Command::SET_RIDER_NODE_ID: {
        if (!req.has_rider_node_id) {
            replyError(req.request_id, boardfinder::ErrorCode::MALFORMED);
            break;
        }
        Prefs next = prefs;
        const bool riderChangedWhileArmed = runtimeArmed && req.rider_node_id != prefs.rider_node_id;
        next.rider_node_id = req.rider_node_id;
        if (!persistIfChanged(next)) {
            replyError(req.request_id, boardfinder::ErrorCode::INTERNAL);
            break;
        }
        if (riderChangedWhileArmed || next.rider_node_id == 0) {
            runtimeArmed = false;
            if (boardFinderEngine)
                boardFinderEngine->disarm();
        }
        applyRuntime(true);
        replyConfig(req.request_id);
        break;
    }

    case boardfinder::Command::ARM_BOARD: {
        if (prefs.device_role != boardfinder::DeviceRole::BOARD) {
            replyError(req.request_id, boardfinder::ErrorCode::NOT_BOARD);
            break;
        }
        if (prefs.rider_node_id == 0) {
            replyError(req.request_id, boardfinder::ErrorCode::RIDER_UNASSIGNED);
            break;
        }
        runtimeArmed = true;
        auto err = boardFinderEngine ? boardFinderEngine->arm() : boardfinder::ErrorCode::INTERNAL;
        if (err != boardfinder::ErrorCode::NONE) {
            runtimeArmed = false;
            replyError(req.request_id, err);
            break;
        }
        replyBoardStatus(req.request_id);
        break;
    }

    case boardfinder::Command::DISARM_BOARD: {
        runtimeArmed = false;
        if (boardFinderEngine)
            boardFinderEngine->disarm();
        replyBoardStatus(req.request_id);
        break;
    }

    case boardfinder::Command::GET_BOARD_STATUS:
        replyBoardStatus(req.request_id);
        break;

    case boardfinder::Command::IDENTIFY_DEVICE:
        boardfinder::startBoardFinderIdentify();
        pendingReply = boardfinder::Message{};
        pendingReply.protocol_version = boardfinder::PROTOCOL_VERSION;
        pendingReply.request_id = req.request_id;
        pendingReply.command = boardfinder::Command::IDENTIFY_RESPONSE;
        pendingReply.status = boardfinder::Status::OK;
        pendingReply.error_code = boardfinder::ErrorCode::NONE;
        hasPendingReply = true;
        break;

    default:
        replyError(req.request_id, boardfinder::ErrorCode::UNSUPPORTED_COMMAND);
        break;
    }

    deliverReply(mp);
    return ProcessMessage::STOP;
}

meshtastic_MeshPacket *BoardFinderModule::allocReply()
{
    return nullptr;
}

#endif
