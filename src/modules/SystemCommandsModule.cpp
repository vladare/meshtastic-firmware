#include "SystemCommandsModule.h"
#include "input/InputBroker.h"
#include "meshUtils.h"

#if HAS_SCREEN
#include "MessageStore.h"
#include "graphics/Screen.h"
#include "graphics/SharedUIDisplay.h"
#endif

#include "GPS.h"
#include "MeshService.h"
#include "Module.h"
#include "NodeDB.h"
#include "main.h"
#include "mesh/Router.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/generated/meshtastic/portnums.pb.h"
#include "modules/AdminModule.h"
#include "modules/ExternalNotificationModule.h"

SystemCommandsModule *systemCommandsModule;

SystemCommandsModule::SystemCommandsModule()
{
    if (inputBroker)
        inputObserver.observe(inputBroker);
}

int SystemCommandsModule::handleInputEvent(const InputEvent *event)
{
    LOG_INPUT("SystemCommands Input event %u! kb %u", event->inputEvent, event->kbchar);
    // System commands (all others fall through)
    switch (event->kbchar) {
    // Fn key symbols
    case INPUT_BROKER_MSG_FN_SYMBOL_ON:
    case INPUT_BROKER_MSG_FN_SYMBOL_OFF:
        return 0;
    // Brightness
    case INPUT_BROKER_MSG_BRIGHTNESS_UP:
        IF_SCREEN(screen->increaseBrightness());
        LOG_DEBUG("Increase Screen Brightness");
        return 0;
    case INPUT_BROKER_MSG_BRIGHTNESS_DOWN:
        IF_SCREEN(screen->decreaseBrightness());
        LOG_DEBUG("Decrease Screen Brightness");
        return 0;
    // Mute
    case INPUT_BROKER_MSG_MUTE_TOGGLE:
        if (moduleConfig.external_notification.enabled && externalNotificationModule) {
            externalNotificationModule->setMute(!externalNotificationModule->getMute());
            IF_SCREEN(if (!externalNotificationModule->getMute()) externalNotificationModule->stopNow(); screen->showSimpleBanner(
                externalNotificationModule->getMute() ? "Notifications\nDisabled" : "Notifications\nEnabled", 3000);)
        }
        return 0;
    // Bluetooth
    case INPUT_BROKER_MSG_BLUETOOTH_TOGGLE:
        config.bluetooth.enabled = !config.bluetooth.enabled;
        LOG_INFO("User toggled Bluetooth");
        nodeDB->saveToDisk();
#if defined(ARDUINO_ARCH_NRF52)
        if (!config.bluetooth.enabled) {
            disableBluetooth();
            IF_SCREEN(screen->showSimpleBanner("Bluetooth OFF\nRebooting", 3000));
            rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 2000;
        } else {
            IF_SCREEN(screen->showSimpleBanner("Bluetooth ON\nRebooting", 3000));
            rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
        }
#else
        if (!config.bluetooth.enabled) {
            disableBluetooth();
            IF_SCREEN(screen->showSimpleBanner("Bluetooth OFF", 3000));
        } else {
            IF_SCREEN(screen->showSimpleBanner("Bluetooth ON\nRebooting", 3000));
            rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
        }
#endif
        return 0;
    case INPUT_BROKER_MSG_REBOOT:
        IF_SCREEN(screen->showSimpleBanner("Rebooting...", 0));
        nodeDB->saveToDisk();
#if HAS_SCREEN
        messageStore.saveToFlash();
#endif
        rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
        // runState = CANNED_MESSAGE_RUN_STATE_INACTIVE;
        return true;
    }

    switch (event->inputEvent) {
        // GPS
    case INPUT_BROKER_GPS_TOGGLE:
#if !MESHTASTIC_EXCLUDE_GPS
        if (gps) {
            if (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED &&
                config.position.fixed_position == false) {
                nodeDB->clearLocalPosition();
                nodeDB->saveToDisk();
            }
            gps->toggleGpsMode();
            const char *msg =
                (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED) ? "GPS Enabled" : "GPS Disabled";
            IF_SCREEN(screen->forceDisplay(); screen->showSimpleBanner(msg, 3000);)
        }
#endif
        return true;
    // Mesh ping
    case INPUT_BROKER_SEND_PING:
        service->refreshLocalMeshNode();
        if (service->trySendPosition(NODENUM_BROADCAST, true)) {
            IF_SCREEN(screen->showSimpleBanner("Position\nSent", 3000));
        } else {
            IF_SCREEN(screen->showSimpleBanner("Node Info\nSent", 3000));
        }
        return true;
    // Power control
    case INPUT_BROKER_SHUTDOWN:
        shutdownAtMsec = millis();
        return true;

    // SOS (T1000-E and any board that maps long-press to INPUT_BROKER_SOS)
    case INPUT_BROKER_SOS: {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(nodeDB->getNodeNum());

        // 1) Keep existing ALERT_APP send (popup + alarm)
        meshtastic_MeshPacket *p = router->allocForSending();
        if (p) {
            p->decoded.portnum = meshtastic_PortNum_ALERT_APP;
            p->priority = meshtastic_MeshPacket_Priority_ALERT;
            static const char sosPayload[] = "SOS";
            memcpy(p->decoded.payload.bytes, sosPayload, sizeof(sosPayload) - 1);
            p->decoded.payload.size = sizeof(sosPayload) - 1;
            if (node) {
                p->channel = node->channel;
            }
            service->sendToMesh(p, RX_SRC_LOCAL);
            IF_SCREEN(screen->showSimpleBanner("SOS Sent", 3000));
            LOG_INFO("SOS sent to mesh");
        }

        // 2) Send text chat message (TEXT_MESSAGE_APP) so it appears in the channel
        //    Use same GPS check as PositionModule: hasLocalPositionSinceBoot() + valid node position
        if (node) {
            bool hasPosition = nodeDB->hasLocalPositionSinceBoot() && nodeDB->hasValidPosition(node);
            char textBuf[200];
            size_t textLen;
            if (hasPosition) {
                double lat = node->position.latitude_i * 1e-7;
                double lon = node->position.longitude_i * 1e-7;
                int n = snprintf(textBuf, sizeof(textBuf), "SOS: I need help. Location: %.5f, %.5f", lat, lon);
                textLen = (n > 0 && (size_t)n < sizeof(textBuf)) ? (size_t)n : 0;
            } else {
                static const char noFix[] = "SOS: I need help. No GPS fix.";
                textLen = sizeof(noFix) - 1;
                memcpy(textBuf, noFix, textLen + 1);
            }
            if (textLen > 0) {
                meshtastic_MeshPacket *tp = router->allocForSending();
                if (tp && textLen <= sizeof(tp->decoded.payload.bytes)) {
                    tp->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
                    tp->channel = node->channel;
                    memcpy(tp->decoded.payload.bytes, textBuf, textLen);
                    tp->decoded.payload.size = textLen;
                    service->sendToMesh(tp, RX_SRC_LOCAL);
                }
            }
            if (hasPosition) {
                service->trySendPosition(NODENUM_BROADCAST, true);
            }
        }
        return true;
    }

    default:
        // No other input events handled here
        break;
    }
    return false;
}
