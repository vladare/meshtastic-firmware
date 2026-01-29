#include "TextMessageModule.h"
#include "MeshService.h"
#include "MessageStore.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "configuration.h"
#include "mesh/MeshTypes.h"
#include "graphics/Screen.h"
#include "graphics/SharedUIDisplay.h"
#include "graphics/draw/MessageRenderer.h"
#include "modules/ExternalNotificationModule.h"
#include "main.h"
TextMessageModule *textMessageModule;

// Receiver-side SOS auto-ack: one SOS gesture sends 2 packets (ALERT_APP "SOS" + TEXT_MESSAGE_APP "SOS: ..."),
// so deduplicate by sender for a short window to prevent ACK spam.
static const uint32_t SOS_ACK_DEDUP_MS = 10 * 1000;
static NodeNum lastSosAckFrom = 0;
static uint32_t lastSosAckAtMs = 0;

static inline bool isSosAckForUs(const meshtastic_MeshPacket &mp)
{
    if (!isToUs(&mp)) {
        return false;
    }
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return false;
    }
    if (mp.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        return false;
    }

    const auto &pl = mp.decoded.payload;
    return (pl.size == 7 && pl.bytes[0] == 'S' && pl.bytes[1] == 'O' && pl.bytes[2] == 'S' && pl.bytes[3] == '-' &&
            pl.bytes[4] == 'A' && pl.bytes[5] == 'C' && pl.bytes[6] == 'K');
}

static bool isSosForAck(const meshtastic_MeshPacket &mp)
{
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return false;
    }

    const auto &pl = mp.decoded.payload;

    // Case A: ALERT_APP "SOS" (exact)
    if (mp.decoded.portnum == meshtastic_PortNum_ALERT_APP) {
        return (pl.size == 3 && pl.bytes[0] == 'S' && pl.bytes[1] == 'O' && pl.bytes[2] == 'S');
    }

    // Case B: TEXT_MESSAGE_APP "SOS: ..."
    if (mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
        return (pl.size >= 4 && pl.bytes[0] == 'S' && pl.bytes[1] == 'O' && pl.bytes[2] == 'S' && pl.bytes[3] == ':');
    }

    return false;
}

static void maybeSendSosAck(const meshtastic_MeshPacket &mp)
{
    if (!isSosForAck(mp)) {
        return;
    }
    if (isFromUs(&mp)) {
        return;
    }
    if (!mp.from || isBroadcast(mp.from)) {
        return;
    }

    const uint32_t now = millis();
    if (mp.from == lastSosAckFrom && (uint32_t)(now - lastSosAckAtMs) < SOS_ACK_DEDUP_MS) {
        return;
    }

    // Build a direct message back to the sender, on the same channel as the incoming SOS.
    meshtastic_MeshPacket *ack = router->allocForSending();
    if (!ack) {
        return;
    }

    ack->to = mp.from;
    ack->channel = mp.channel; // critical: keep same channel so sender can decrypt/show it
    ack->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;

    static const char payload[] = "SOS-ACK"; // must not be "SOS" or start with "SOS:" to avoid loops
    const size_t len = sizeof(payload) - 1;
    if (len > sizeof(ack->decoded.payload.bytes)) {
        packetPool.release(ack);
        return;
    }
    memcpy(ack->decoded.payload.bytes, payload, len);
    ack->decoded.payload.size = len;

    service->sendToMesh(ack, RX_SRC_LOCAL);
    lastSosAckFrom = mp.from;
    lastSosAckAtMs = now;
}

ProcessMessage TextMessageModule::handleReceived(const meshtastic_MeshPacket &mp)
{
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
    auto &p = mp.decoded;
    LOG_INFO("Received text msg from=0x%0x, id=0x%x, msg=%.*s", mp.from, mp.id, p.payload.size, p.payload.bytes);
#endif
    // SOS auto-ack is receiver-side logic and should not depend on notification settings.
    maybeSendSosAck(mp);

    // Sender-side: schedule delayed "ta-daa" (~2-3 s after SOS) so user hears "someone received it," not immediate chirp.
    if (isSosAckForUs(mp)) {
        if (externalNotificationModule) {
            externalNotificationModule->scheduleSosAckPlayback();
        }
    }

    // add packet ID to the rolling list of packets
    textPacketList[textPacketListIndex] = mp.id;
    textPacketListIndex = (textPacketListIndex + 1) % TEXT_PACKET_LIST_SIZE;

    // We only store/display messages destined for us.
    devicestate.rx_text_message = mp;
    devicestate.has_rx_text_message = true;
    IF_SCREEN(
        // Guard against running in MeshtasticUI or with no screen
        if (config.display.displaymode != meshtastic_Config_DisplayConfig_DisplayMode_COLOR) {
            // Store in the central message history
            const StoredMessage &sm = messageStore.addFromPacket(mp);

            // Pass message to renderer (banner + thread switching + scroll reset)
            // Use the global Screen singleton to retrieve the current OLED display
            auto *display = screen ? screen->getDisplayDevice() : nullptr;
            graphics::MessageRenderer::handleNewMessage(display, sm, mp);
        })
    // Only trigger screen wake if configuration allows it
    if (shouldWakeOnReceivedMessage()) {
        powerFSM.trigger(EVENT_RECEIVED_MSG);
    }

    // Notify any observers (e.g. external modules that care about packets)
    notifyObservers(&mp);

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}

bool TextMessageModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return MeshService::isTextPayload(p);
}

bool TextMessageModule::recentlySeen(uint32_t id)
{
    for (size_t i = 0; i < TEXT_PACKET_LIST_SIZE; i++) {
        if (textPacketList[i] != 0 && textPacketList[i] == id) {
            return true;
        }
    }
    return false;
}