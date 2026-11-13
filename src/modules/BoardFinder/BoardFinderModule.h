#pragma once

#if defined(TRACKER_T1000_E)

#include "SinglePortModule.h"
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
    static concurrency::Lock &prefsLock();
    static bool loadPrefs(bool &enabled);
    static bool savePrefs(bool enabled);
    static bool isLocalPhoneConfigRequest(const meshtastic_MeshPacket &mp);

    void replyConfig(uint32_t requestId, bool enabled);
    void replyError(uint32_t requestId, boardfinder::ErrorCode code);

    boardfinder::Message pendingReply{};
    bool hasPendingReply = false;
};

extern BoardFinderModule *boardFinderModule;

#endif
