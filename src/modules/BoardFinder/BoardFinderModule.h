#pragma once

#if defined(TRACKER_T1000_E)

#include "BoardFinderEngine.h"
#include "SinglePortModule.h"
#include "boardfinder_economy.h"
#include "boardfinder_protocol.h"
#include "concurrency/Lock.h"
#include "mesh/MeshTypes.h"

/**
 * Local-only Board Finder configuration over PortNum.PRIVATE_APP.
 * Accepts Phone API packets (from==0, unicast to this node); rejects remote LoRa control.
 */
class BoardFinderModule : public SinglePortModule
{
  public:
    BoardFinderModule();

  protected:
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    meshtastic_MeshPacket *allocReply() override;

  private:
    // Persistent fields only. board_armed is runtime-only (never restored from flash).
    struct Prefs {
        bool ble_burst_enabled = false;
        boardfinder::DeviceRole device_role = boardfinder::DeviceRole::NORMAL;
        uint32_t rider_node_id = 0;
        boardfinder::EconomySettings economy = boardfinder::defaultEconomySettings();
    };

    // Runtime ARM mirror for CONFIG_RESPONSE; source of truth is BoardFinderEngine.
    bool runtimeArmed = false;

    static concurrency::Lock &prefsLock();
    static bool loadPrefs(Prefs &out);
    static bool savePrefs(const Prefs &in);
    static bool isLocalPhoneConfigRequest(const meshtastic_MeshPacket &mp);

    void applyRuntime(bool restartIfArmed);
    bool persistIfChanged(const Prefs &next);
    void fillEconomyFields(boardfinder::Message &msg) const;
    void replyError(uint32_t requestId, boardfinder::ErrorCode code);
    void replyConfig(uint32_t requestId);
    void replyBoardStatus(uint32_t requestId);
    void deliverReply(const meshtastic_MeshPacket &mp);

    Prefs prefs{};
    boardfinder::Message pendingReply{};
    bool hasPendingReply = false;
};

extern BoardFinderModule *boardFinderModule;

#endif
