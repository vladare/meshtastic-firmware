#if defined(TRACKER_T1000_E)

#include "BoardFinderModule.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "SPILock.h"
#include "power/PowerHAL.h"
#include "SafeFile.h"
#include "configuration.h"
#include "main.h"
#include "platform/nrf52/T1000eBleBurst.h"
#include <cstring>

BoardFinderModule *boardFinderModule;

namespace
{
constexpr const char *kPrefsPath = "/prefs/boardfinder.proto";
constexpr uint32_t kPrefsMagic = 0xBF100001u;
constexpr uint32_t kPrefsVersion = 1;

struct PrefsRecord {
    uint32_t magic;
    uint32_t version;
    uint8_t ble_burst_enabled;
    uint8_t reserved[3];
};
} // namespace

concurrency::Lock &BoardFinderModule::prefsLock()
{
    static concurrency::Lock lock;
    return lock;
}

BoardFinderModule::BoardFinderModule() : SinglePortModule("BoardFinder", meshtastic_PortNum_PRIVATE_APP)
{
    bool enabled = false;
    loadPrefs(enabled);
    t1000eBurstSetEnabled(enabled);
    LOG_INFO("BoardFinder module ready, ble_burst_enabled=%d", enabled ? 1 : 0);
}

bool BoardFinderModule::isLocalPhoneConfigRequest(const meshtastic_MeshPacket &mp)
{
    // from==0 is the Phone API local convention (same as AdminModule).
    // RadioLibInterface drops air packets with from==0, so LoRa spoof of local Node ID
    // cannot pass this gate. Not a cryptographic Phone API proof.
    if (mp.from != 0)
        return false;
    if (isBroadcast(mp.to) || !isToUs(&mp))
        return false;
    return true;
}

bool BoardFinderModule::loadPrefs(bool &enabled)
{
    enabled = false;
    concurrency::LockGuard guard(&prefsLock());
#ifdef FSCom
    concurrency::LockGuard spi(spiLock);
    if (!FSCom.exists(kPrefsPath))
        return true;

    auto f = FSCom.open(kPrefsPath, FILE_O_READ);
    if (!f)
        return false;

    PrefsRecord rec{};
    const size_t n = f.read(reinterpret_cast<uint8_t *>(&rec), sizeof(rec));
    f.close();
    if (n != sizeof(rec) || rec.magic != kPrefsMagic || rec.version == 0 || rec.version > kPrefsVersion) {
        LOG_WARN("BoardFinder prefs invalid; defaulting OFF");
        enabled = false;
        return false;
    }
    enabled = rec.ble_burst_enabled != 0;
    return true;
#else
    (void)enabled;
    return true;
#endif
}

bool BoardFinderModule::savePrefs(bool enabled)
{
    concurrency::LockGuard guard(&prefsLock());
#ifdef FSCom
    if (!powerHAL_isPowerLevelSafe()) {
        LOG_ERROR("BoardFinder prefs save blocked: unsafe power");
        return false;
    }

    PrefsRecord rec{};
    rec.magic = kPrefsMagic;
    rec.version = kPrefsVersion;
    rec.ble_burst_enabled = enabled ? 1 : 0;

    SafeFile f(kPrefsPath, true);
    size_t written = f.write(reinterpret_cast<const uint8_t *>(&rec), sizeof(rec));
    bool ok = f.close() && written == sizeof(rec);
    if (!ok)
        LOG_ERROR("BoardFinder prefs save failed");
    return ok;
#else
    (void)enabled;
    return false;
#endif
}

void BoardFinderModule::replyConfig(uint32_t requestId, bool enabled)
{
    pendingReply = boardfinder::Message{};
    pendingReply.protocol_version = boardfinder::PROTOCOL_VERSION;
    pendingReply.request_id = requestId;
    pendingReply.command = boardfinder::Command::CONFIG_RESPONSE;
    pendingReply.ble_burst_enabled = enabled;
    pendingReply.has_ble_burst_enabled = true;
    pendingReply.status = boardfinder::Status::OK;
    pendingReply.error_code = boardfinder::ErrorCode::NONE;
    hasPendingReply = true;
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

ProcessMessage BoardFinderModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        mp.decoded.portnum != meshtastic_PortNum_PRIVATE_APP) {
        return ProcessMessage::CONTINUE;
    }

    hasPendingReply = false;

    if (!isLocalPhoneConfigRequest(mp)) {
        LOG_WARN("BoardFinder rejected non-local PRIVATE_APP config packet from=0x%x to=0x%x", mp.from, mp.to);
        // No reply over LoRa for remote attempts.
        return ProcessMessage::STOP;
    }

    boardfinder::Message req{};
    if (!boardfinder::decodeMessage(mp.decoded.payload.bytes, mp.decoded.payload.size, req)) {
        replyError(0, boardfinder::ErrorCode::MALFORMED);
        return ProcessMessage::STOP;
    }

    if (req.protocol_version == 0 || req.protocol_version > boardfinder::PROTOCOL_VERSION) {
        replyError(req.request_id, boardfinder::ErrorCode::UNSUPPORTED_VERSION);
        return ProcessMessage::STOP;
    }

    switch (req.command) {
    case boardfinder::Command::GET_CONFIG: {
        bool enabled = t1000eBurstIsEnabled();
        replyConfig(req.request_id, enabled);
        break;
    }
    case boardfinder::Command::SET_CONFIG: {
        if (!req.has_ble_burst_enabled) {
            replyError(req.request_id, boardfinder::ErrorCode::MALFORMED);
            break;
        }
        const bool current = t1000eBurstIsEnabled();
        if (current != req.ble_burst_enabled) {
            if (!savePrefs(req.ble_burst_enabled)) {
                replyError(req.request_id, boardfinder::ErrorCode::INTERNAL);
                break;
            }
            t1000eBurstSetEnabled(req.ble_burst_enabled);
            t1000eBurstReapply();
        }
        replyConfig(req.request_id, t1000eBurstIsEnabled());
        break;
    }
    default:
        replyError(req.request_id, boardfinder::ErrorCode::UNSUPPORTED_COMMAND);
        break;
    }

    // Deliver only to the local phone queue. Never call sendToMesh for config responses.
    if (hasPendingReply && service != nullptr && nodeDB != nullptr) {
        uint8_t buf[boardfinder::MAX_MESSAGE_BYTES];
        size_t len = boardfinder::encodeMessage(pendingReply, buf, sizeof(buf));
        hasPendingReply = false;
        if (len != 0) {
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
    }

    return ProcessMessage::STOP;
}

meshtastic_MeshPacket *BoardFinderModule::allocReply()
{
    // Responses are queued directly with sendToPhone above.
    return nullptr;
}

#endif
