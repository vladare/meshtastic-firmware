/**
 * @file ExternalNotificationModule.cpp
 * @brief Implementation of the ExternalNotificationModule class.
 *
 * This file contains the implementation of the ExternalNotificationModule class, which is responsible for handling external
 * notifications such as vibration, buzzer, and LED lights. The class provides methods to turn on and off the external
 * notification outputs and to play ringtones using PWM buzzer. It also includes default configurations and a runOnce() method to
 * handle the module's behavior.
 *
 * Documentation:
 * https://meshtastic.org/docs/configuration/module/external-notification
 *
 * @author Jm Casler & Meshtastic Team
 * @date [Insert Date]
 */
#include "ExternalNotificationModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RTC.h"
#include "Router.h"
#include "configuration.h"
#include "main.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/generated/meshtastic/rtttl.pb.h"
#include <Arduino.h>

#ifdef HAS_NCP5623
#include <graphics/RAKled.h>
#endif

#ifdef HAS_LP5562
#include <graphics/NomadStarLED.h>
#endif

#ifdef HAS_NEOPIXEL
#include <graphics/NeoPixel.h>
#endif

#ifdef UNPHONE
#include "unPhone.h"
extern unPhone unphone;
#endif

#if defined(HAS_RGB_LED)
uint8_t red = 0;
uint8_t green = 0;
uint8_t blue = 0;
uint8_t white = 0;
uint8_t colorState = 1;
uint8_t brightnessIndex = 0;
uint8_t brightnessValues[] = {0, 10, 20, 30, 50, 90, 160, 170}; // blue gets multiplied by 1.5
bool ascending = true;
#endif

#ifndef PIN_BUZZER
#define PIN_BUZZER false
#endif

/*
    Documentation:
        https://meshtastic.org/docs/configuration/module/external-notification
*/

// Default configurations
#ifdef EXT_NOTIFY_OUT
#define EXT_NOTIFICATION_MODULE_OUTPUT EXT_NOTIFY_OUT
#else
#define EXT_NOTIFICATION_MODULE_OUTPUT 0
#endif
#define EXT_NOTIFICATION_MODULE_OUTPUT_MS 1000

#define EXT_NOTIFICATION_FAST_THREAD_MS 25

#define ASCII_BELL 0x07

// Distinct SOS sound: three-note repeating alarm pattern, clearly different from generic message ringtone.
// Used when the packet is classified as SOS (ALERT_APP "SOS" or TEXT_MESSAGE_APP "SOS: ...").
static const char SOS_RINGTONE[] = "SOS:d=4,o=5,b=180:c6,g5,c6,g5,c6,g5,c6,g5,c6";

// SOS repeating schedule:
// - Phase 1 (first 15 minutes): repeat every 5 minutes, max 3 repeats.
// - Phase 2 (after 15 minutes OR after 3 repeats): repeat every 15 minutes.
static const uint32_t SOS_FAST_PHASE_MS = 15 * 60 * 1000;
static const uint32_t SOS_FAST_REPEAT_MS = 5 * 60 * 1000;
static const uint32_t SOS_SLOW_REPEAT_MS = 15 * 60 * 1000;
static const uint8_t SOS_FAST_REPEAT_MAX = 3;
// Deduplicate the 2-packet SOS gesture (ALERT + TEXT) so we don't start twice.
static const uint32_t SOS_START_DEDUP_MS = 10 * 1000;

// Sender-side SOS-ACK delayed feedback (ta-daa ~2–3 s after SOS).
static const uint32_t ACK_DELAY_MS = 2500;
static const uint32_t ACK_MIN_AFTER_SOS_MS = 2000;
static const uint32_t ACK_MAX_WAIT_MS = 10000;
static const char SOS_ACK_RINGTONE[] = "ACK:d=16,o=5,b=180:b5,16p,a6";

// Sender-side state for one ACK sound per SOS gesture.
static uint32_t lastSosSentAtMs = 0;
static bool ackPlayedForCurrentSos = false;
static bool pendingAckPlayback = false;
static uint32_t ackReceivedAtMs = 0;
static uint32_t ackPlayAtMs = 0;

/** Sender-side: play the "ta-daa" ACK confirmation once. Respects enabled, alert_message_buzzer, mute, canBuzz(). */
static void playSosAckSound(ExternalNotificationModule *mod)
{
    if (!mod || !moduleConfig.external_notification.enabled)
        return;
    if (!moduleConfig.external_notification.alert_message_buzzer || mod->getMute())
        return;
    if (!mod->canBuzz())
        return;
    if (moduleConfig.external_notification.use_i2s_as_buzzer) {
#ifdef HAS_I2S
        if (audioThread)
            audioThread->beginRttl(SOS_ACK_RINGTONE, strlen(SOS_ACK_RINGTONE));
#endif
        return;
    }
    if (moduleConfig.external_notification.use_pwm && config.device.buzzer_gpio) {
        rtttl::begin(config.device.buzzer_gpio, SOS_ACK_RINGTONE);
        return;
    }
    // Active buzzer / digital pin: two-pulse "ta-daa" (100 ms, pause 100 ms, 180 ms). Works on all architectures
    // (tone() is unavailable on ESP32/RP2040/PORTDUINO; setExternalState is the universal fallback).
    mod->setExternalState(2, true);
    delay(100);
    mod->setExternalState(2, false);
    delay(100);
    mod->setExternalState(2, true);
    delay(180);
    mod->setExternalState(2, false);
}

/**
 * @brief Returns true iff the packet is an SOS alert for notification sound purposes.
 * Covers: (A) ALERT_APP with exact payload "SOS" (3 bytes), and
 *         (B) TEXT_MESSAGE_APP with payload starting with "SOS:" (e.g. "SOS: I need help. No GPS fix.").
 * Used to route receive-side notifications to the distinct SOS sound; non-SOS messages use the generic beep.
 */
static bool isSosAlert(const meshtastic_MeshPacket &mp)
{
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return false;
    }
    const auto &pl = mp.decoded.payload;

    // Case A: ALERT_APP "SOS"
    if (mp.decoded.portnum == meshtastic_PortNum_ALERT_APP) {
        if (pl.size == 3 && pl.bytes[0] == 'S' && pl.bytes[1] == 'O' && pl.bytes[2] == 'S') {
            return true;
        }
        return false;
    }

    // Case B: TEXT_MESSAGE_APP "SOS: ..."
    if (mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
        if (pl.size >= 4 && pl.bytes[0] == 'S' && pl.bytes[1] == 'O' && pl.bytes[2] == 'S' && pl.bytes[3] == ':') {
            return true;
        }
    }

    return false;
}

static bool isSosAckDmToUs(const meshtastic_MeshPacket &mp)
{
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
        return false;
    }
    if (!isToUs(&mp) || isBroadcast(mp.to)) {
        return false;
    }
    if (mp.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        return false;
    }
    const auto &pl = mp.decoded.payload;
    return (pl.size == 7 && pl.bytes[0] == 'S' && pl.bytes[1] == 'O' && pl.bytes[2] == 'S' && pl.bytes[3] == '-' &&
            pl.bytes[4] == 'A' && pl.bytes[5] == 'C' && pl.bytes[6] == 'K');
}

// Type of sound to replay during nag cycle; runOnce() uses this so SOS replays SOS_RINGTONE, not generic ringtone.
enum NagSoundType { NAG_SOUND_NORMAL = 0, NAG_SOUND_SOS = 1 };
static uint8_t currentNagSound = NAG_SOUND_NORMAL;

// Receiver-side SOS repeating state (local device).
static bool sosActive = false;
static NodeNum sosFrom = 0;
static uint32_t nextRepeatAtMs = 0;
static uint32_t sosStartAtMs = 0;
static uint8_t sosRepeatCount = 0;
static uint32_t lastSosStartAtMs = 0;

meshtastic_RTTTLConfig rtttlConfig;

ExternalNotificationModule *externalNotificationModule;

bool externalCurrentState[3] = {};

uint32_t externalTurnedOn[3] = {};

static const char *rtttlConfigFile = "/prefs/ringtone.proto";

int32_t ExternalNotificationModule::runOnce()
{
    if (!moduleConfig.external_notification.enabled) {
        return INT32_MAX; // we don't need this thread here...
    } else {
        uint32_t delay = EXT_NOTIFICATION_MODULE_OUTPUT_MS;
        bool isRtttlPlaying = rtttl::isPlaying();
#ifdef HAS_I2S
        // audioThread->isPlaying() also handles actually playing the RTTTL, needs to be called in loop
        isRtttlPlaying = isRtttlPlaying || audioThread->isPlaying();
#endif
        const uint32_t now = millis();

        // Sender-side: delayed SOS-ACK playback (one "ta-daa" per SOS gesture).
        if (pendingAckPlayback) {
            if ((uint32_t)(now - ackReceivedAtMs) > ACK_MAX_WAIT_MS) {
                LOG_DEBUG("SOS-ACK playback cancelled (timeout)");
                pendingAckPlayback = false;
            } else if ((int32_t)(now - ackPlayAtMs) >= 0) {
                playSosAckSound(this);
                LOG_DEBUG("SOS-ACK played (delayed)");
                ackPlayedForCurrentSos = true;
                pendingAckPlayback = false;
                setIntervalFromNow(0);
            }
        }

        if ((nagCycleCutoff < now) && !isRtttlPlaying) {
            // Turn off external notification immediately when timeout is reached, regardless of song state
            nagCycleCutoff = UINT32_MAX;
            ExternalNotificationModule::stopNow();
            isNagging = false;
            if (!sosActive) {
                return INT32_MAX; // save cycles till we're needed again
            }
        }

        // Repeat SOS alarm while active, regardless of other incoming packets.
        const bool sosSoundingNow = (isNagging && currentNagSound == NAG_SOUND_SOS && nagCycleCutoff != UINT32_MAX &&
                                     (int32_t)(nagCycleCutoff - now) > 0);
        if (sosActive && nextRepeatAtMs != 0 && (int32_t)(now - nextRepeatAtMs) >= 0 && !sosSoundingNow) {
            // Replay the same SOS alarm now.
            isNagging = true;
            currentNagSound = NAG_SOUND_SOS;

            if (canBuzz()) {
                if (!moduleConfig.external_notification.use_pwm && !moduleConfig.external_notification.use_i2s_as_buzzer) {
                    setExternalState(2, true);
                } else {
#ifdef HAS_I2S
                    if (moduleConfig.external_notification.use_i2s_as_buzzer) {
                        if (audioThread) {
                            audioThread->beginRttl(SOS_RINGTONE, strlen(SOS_RINGTONE));
                        }
                    } else
#endif
                        if (moduleConfig.external_notification.use_pwm) {
                        rtttl::begin(config.device.buzzer_gpio, SOS_RINGTONE);
                    }
                }
            }

            if (moduleConfig.external_notification.nag_timeout) {
                nagCycleCutoff = now + moduleConfig.external_notification.nag_timeout * 1000;
            } else {
                uint32_t ms = moduleConfig.external_notification.output_ms ? moduleConfig.external_notification.output_ms * 3 : 3000;
                nagCycleCutoff = now + ms;
            }

            setIntervalFromNow(0);

            // Count repeats (does not include the initial alarm on detection).
            if (sosRepeatCount < 0xFF) {
                sosRepeatCount++;
            }
            if (sosActive && sosStartAtMs == 0) {
                sosStartAtMs = now;
            }
            const uint32_t elapsed = now - sosStartAtMs;
            const bool useFastNext = (elapsed < SOS_FAST_PHASE_MS) && (sosRepeatCount < SOS_FAST_REPEAT_MAX);
            nextRepeatAtMs = now + (useFastNext ? SOS_FAST_REPEAT_MS : SOS_SLOW_REPEAT_MS);
        }

        // If the output is turned on, turn it back off after the given period of time.
        if (isNagging) {
            delay = (moduleConfig.external_notification.output_ms ? moduleConfig.external_notification.output_ms
                                                                  : EXT_NOTIFICATION_MODULE_OUTPUT_MS);
            if (externalTurnedOn[0] + delay < now) {
                setExternalState(0, !getExternal(0));
            }
            if (externalTurnedOn[1] + delay < now) {
                setExternalState(1, !getExternal(1));
            }
            // Only toggle buzzer output if not using PWM mode (to avoid conflict with RTTTL)
            if (!moduleConfig.external_notification.use_pwm && externalTurnedOn[2] + delay < now) {
                LOG_DEBUG("EXTERNAL 2 %d compared to %d", externalTurnedOn[2] + moduleConfig.external_notification.output_ms,
                          millis());
                setExternalState(2, !getExternal(2));
            }
#if defined(HAS_RGB_LED)
            red = (colorState & 4) ? brightnessValues[brightnessIndex] : 0;          // Red enabled on colorState = 4,5,6,7
            green = (colorState & 2) ? brightnessValues[brightnessIndex] : 0;        // Green enabled on colorState = 2,3,6,7
            blue = (colorState & 1) ? (brightnessValues[brightnessIndex] * 1.5) : 0; // Blue enabled on colorState = 1,3,5,7
            white = (colorState & 12) ? brightnessValues[brightnessIndex] : 0;
#ifdef HAS_NCP5623
            if (rgb_found.type == ScanI2C::NCP5623) {
                rgb.setColor(red, green, blue);
            }
#endif
#ifdef HAS_LP5562
            if (rgb_found.type == ScanI2C::LP5562) {
                rgbw.setColor(red, green, blue, white);
            }
#endif
#ifdef RGBLED_CA
            analogWrite(RGBLED_RED, 255 - red); // CA type needs reverse logic
            analogWrite(RGBLED_GREEN, 255 - green);
            analogWrite(RGBLED_BLUE, 255 - blue);
#elif defined(RGBLED_RED)
            analogWrite(RGBLED_RED, red);
            analogWrite(RGBLED_GREEN, green);
            analogWrite(RGBLED_BLUE, blue);
#endif
#ifdef HAS_NEOPIXEL
            pixels.fill(pixels.Color(red, green, blue), 0, NEOPIXEL_COUNT);
            pixels.show();
#endif
#ifdef UNPHONE
            unphone.rgb(red, green, blue);
#endif
            if (ascending) { // fade in
                brightnessIndex++;
                if (brightnessIndex == (sizeof(brightnessValues) - 1)) {
                    ascending = false;
                }
            } else {
                brightnessIndex--; // fade out
            }
            if (brightnessIndex == 0) {
                ascending = true;
                colorState++; // next color
                if (colorState > 7) {
                    colorState = 1;
                }
            }
            // we need fast updates for the color change
            delay = EXT_NOTIFICATION_FAST_THREAD_MS;
#endif

#ifdef HAS_DRV2605
            drv.go();
#endif
        }

        // Play RTTTL over i2s audio interface if enabled as buzzer
#ifdef HAS_I2S
        if (moduleConfig.external_notification.use_i2s_as_buzzer) {
            if (audioThread->isPlaying()) {
                // Continue playing
            } else if (isNagging && (nagCycleCutoff >= millis())) {
                // Replay using same sound type as initial trigger (SOS vs normal), so SOS never falls back to generic.
                if (currentNagSound == NAG_SOUND_SOS) {
                    audioThread->beginRttl(SOS_RINGTONE, strlen(SOS_RINGTONE));
                } else {
                    audioThread->beginRttl(rtttlConfig.ringtone, strlen_P(rtttlConfig.ringtone));
                }
            }
            // we need fast updates to play the RTTTL
            delay = EXT_NOTIFICATION_FAST_THREAD_MS;
        }
#endif
        // now let the PWM buzzer play
        if (moduleConfig.external_notification.use_pwm && config.device.buzzer_gpio && canBuzz()) {
            if (rtttl::isPlaying()) {
                rtttl::play();
            } else if (isNagging && (nagCycleCutoff >= millis())) {
                // Replay using same sound type as initial trigger (SOS vs normal), so SOS never falls back to generic.
                if (currentNagSound == NAG_SOUND_SOS) {
                    rtttl::begin(config.device.buzzer_gpio, SOS_RINGTONE);
                } else {
                    rtttl::begin(config.device.buzzer_gpio, rtttlConfig.ringtone);
                }
            }
            // we need fast updates to play the RTTTL
            delay = EXT_NOTIFICATION_FAST_THREAD_MS;
        }

        // If SOS repeating is active, ensure we wake up for the next repeat.
        if (sosActive && nextRepeatAtMs != 0) {
            int32_t untilRepeatMs = (int32_t)(nextRepeatAtMs - millis());
            if (untilRepeatMs < 0) {
                untilRepeatMs = 0;
            }
            delay = min(delay, (uint32_t)untilRepeatMs);
        }

        // Sender-side: wake in time for ACK playback or timeout.
        if (pendingAckPlayback) {
            int32_t untilPlayMs = (int32_t)(ackPlayAtMs - millis());
            if (untilPlayMs < 0)
                untilPlayMs = 0;
            uint32_t untilTimeoutMs = ACK_MAX_WAIT_MS - (millis() - ackReceivedAtMs);
            if ((int32_t)untilTimeoutMs < 0)
                untilTimeoutMs = 0;
            delay = min(delay, min((uint32_t)untilPlayMs, untilTimeoutMs));
            if (delay < EXT_NOTIFICATION_FAST_THREAD_MS)
                delay = EXT_NOTIFICATION_FAST_THREAD_MS;
        }

        return delay;
    }
}

/**
 * Based on buzzer mode, return true if we can buzz.
 */
bool ExternalNotificationModule::canBuzz()
{
    if (config.device.buzzer_mode != meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED &&
        config.device.buzzer_mode != meshtastic_Config_DeviceConfig_BuzzerMode_SYSTEM_ONLY) {
        return true;
    }
    return false;
}

bool ExternalNotificationModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return MeshService::isTextPayload(p);
}

void ExternalNotificationModule::onSosSent()
{
    lastSosSentAtMs = millis();
    ackPlayedForCurrentSos = false;
    pendingAckPlayback = false;
    LOG_DEBUG("SOS triggered (sender), ACK state reset");
}

void ExternalNotificationModule::scheduleSosAckPlayback()
{
    if (ackPlayedForCurrentSos) {
        LOG_DEBUG("SOS-ACK ignored (already played for this SOS)");
        return;
    }
    const uint32_t now = millis();
    ackReceivedAtMs = now;
    const uint32_t fromAck = now + ACK_DELAY_MS;
    const uint32_t fromSos = lastSosSentAtMs + ACK_MIN_AFTER_SOS_MS;
    ackPlayAtMs = (fromAck > fromSos) ? fromAck : fromSos;
    pendingAckPlayback = true;
    setIntervalFromNow(0);
    LOG_DEBUG("SOS-ACK scheduled at %lu ms", (unsigned long)ackPlayAtMs);
}

/**
 * Sets the external notification for the specified index.
 *
 * @param index The index of the external notification to change state.
 * @param on Whether we are turning things on (true) or off (false).
 */
void ExternalNotificationModule::setExternalState(uint8_t index, bool on)
{
    externalCurrentState[index] = on;
    externalTurnedOn[index] = millis();

    switch (index) {
    case 1:
#ifdef UNPHONE
        unphone.vibe(on); // the unPhone's vibration motor is on a i2c GPIO expander
#endif
        if (moduleConfig.external_notification.output_vibra)
            digitalWrite(moduleConfig.external_notification.output_vibra, on);
        break;
    case 2:
        // Only control buzzer pin digitally if not using PWM mode
        if (moduleConfig.external_notification.output_buzzer && !moduleConfig.external_notification.use_pwm)
            digitalWrite(moduleConfig.external_notification.output_buzzer, on);
        break;
    default:
        if (output > 0)
            digitalWrite(output, (moduleConfig.external_notification.active ? on : !on));
        break;
    }

#if defined(HAS_RGB_LED)
    if (!on) {
        red = 0;
        green = 0;
        blue = 0;
        white = 0;
    }
#endif

#ifdef HAS_NCP5623
    if (rgb_found.type == ScanI2C::NCP5623) {
        rgb.setColor(red, green, blue);
    }
#endif
#ifdef HAS_LP5562
    if (rgb_found.type == ScanI2C::LP5562) {
        rgbw.setColor(red, green, blue, white);
    }
#endif
#ifdef RGBLED_CA
    analogWrite(RGBLED_RED, 255 - red); // CA type needs reverse logic
    analogWrite(RGBLED_GREEN, 255 - green);
    analogWrite(RGBLED_BLUE, 255 - blue);
#elif defined(RGBLED_RED)
    analogWrite(RGBLED_RED, red);
    analogWrite(RGBLED_GREEN, green);
    analogWrite(RGBLED_BLUE, blue);
#endif
#ifdef HAS_NEOPIXEL
    pixels.fill(pixels.Color(red, green, blue), 0, NEOPIXEL_COUNT);
    pixels.show();
#endif
#ifdef UNPHONE
    unphone.rgb(red, green, blue);
#endif
#ifdef HAS_DRV2605
    if (on) {
        drv.go();
    } else {
        drv.stop();
    }
#endif
}

bool ExternalNotificationModule::getExternal(uint8_t index)
{
    return externalCurrentState[index];
}

// Allow other firmware components to determine whether a notification is ongoing
bool ExternalNotificationModule::nagging()
{
    return isNagging;
}

void ExternalNotificationModule::stopNow()
{
    const bool wasSosNagging =
        (sosActive && isNagging && currentNagSound == NAG_SOUND_SOS && nagCycleCutoff != UINT32_MAX);

    LOG_INFO("Turning off external notification: ");
    LOG_INFO("Stop RTTTL playback");
    rtttl::stop();
#ifdef HAS_I2S
    LOG_INFO("Stop audioThread playback");
    audioThread->stop();
#endif
    // Turn off all outputs
    LOG_INFO("Turning off setExternalStates");
    for (int i = 0; i < 3; i++) {
        setExternalState(i, false);
        externalTurnedOn[i] = 0;
    }
    setIntervalFromNow(0);
#ifdef HAS_DRV2605
    drv.stop();
#endif

    // Prevent the state machine from immediately re-triggering outputs after a manual stop.
    isNagging = false;
    nagCycleCutoff = UINT32_MAX;
    currentNagSound = NAG_SOUND_NORMAL;

    // If we just silenced an active SOS nag, fully cancel the repeat schedule too.
    if (wasSosNagging) {
        sosActive = false;
        sosFrom = 0;
        nextRepeatAtMs = 0;
        sosStartAtMs = 0;
        sosRepeatCount = 0;
        lastSosStartAtMs = 0;
    }

#ifdef HAS_I2S
    // GPIO0 is used as mclk for I2S audio and set to OUTPUT by the sound library
    // T-Deck uses GPIO0 as trackball button, so restore the mode
#if defined(T_DECK) || (defined(BUTTON_PIN) && BUTTON_PIN == 0)
    pinMode(0, INPUT);
#endif
#endif
}

ExternalNotificationModule::ExternalNotificationModule()
    : SinglePortModule("ExternalNotificationModule", meshtastic_PortNum_TEXT_MESSAGE_APP),
      concurrency::OSThread("ExternalNotification")
{
    /*
        Uncomment the preferences below if you want to use the module
        without having to configure it from the PythonAPI or WebUI.
    */

    // moduleConfig.external_notification.alert_message = true;
    // moduleConfig.external_notification.alert_message_buzzer = true;
    // moduleConfig.external_notification.alert_message_vibra = true;
    // moduleConfig.external_notification.use_i2s_as_buzzer = true;

    // moduleConfig.external_notification.active = true;
    // moduleConfig.external_notification.alert_bell = 1;
    // moduleConfig.external_notification.output_ms = 1000;
    // moduleConfig.external_notification.output = 4; // RAK4631 IO4
    // moduleConfig.external_notification.output_buzzer = 10; // RAK4631 IO6
    // moduleConfig.external_notification.output_vibra = 28; // RAK4631 IO7
    // moduleConfig.external_notification.nag_timeout = 300;

    // T-Watch / T-Deck i2s audio as buzzer:
    // moduleConfig.external_notification.enabled = true;
    // moduleConfig.external_notification.nag_timeout = 300;
    // moduleConfig.external_notification.output_ms = 1000;
    // moduleConfig.external_notification.use_i2s_as_buzzer = true;
    // moduleConfig.external_notification.alert_message_buzzer = true;

    if (moduleConfig.external_notification.enabled) {
#if !defined(MESHTASTIC_EXCLUDE_INPUTBROKER)
        if (inputBroker) // put our callback in the inputObserver list
            inputObserver.observe(inputBroker);
#endif
        if (nodeDB->loadProto(rtttlConfigFile, meshtastic_RTTTLConfig_size, sizeof(meshtastic_RTTTLConfig),
                              &meshtastic_RTTTLConfig_msg, &rtttlConfig) != LoadFileResult::LOAD_SUCCESS) {
            memset(rtttlConfig.ringtone, 0, sizeof(rtttlConfig.ringtone));
            // The default ringtone is always loaded from userPrefs.jsonc
            strncpy(rtttlConfig.ringtone, USERPREFS_RINGTONE_RTTTL, sizeof(rtttlConfig.ringtone));
        }

        LOG_INFO("Init External Notification Module");

        output = moduleConfig.external_notification.output ? moduleConfig.external_notification.output
                                                           : EXT_NOTIFICATION_MODULE_OUTPUT;

        // Set the direction of a pin
        if (output > 0) {
            LOG_INFO("Use Pin %i in digital mode", output);
            pinMode(output, OUTPUT);
        }
        setExternalState(0, false);
        externalTurnedOn[0] = 0;
        if (moduleConfig.external_notification.output_vibra) {
            LOG_INFO("Use Pin %i for vibra motor", moduleConfig.external_notification.output_vibra);
            pinMode(moduleConfig.external_notification.output_vibra, OUTPUT);
            setExternalState(1, false);
            externalTurnedOn[1] = 0;
        }
        if (moduleConfig.external_notification.output_buzzer && canBuzz()) {
            if (!moduleConfig.external_notification.use_pwm) {
                LOG_INFO("Use Pin %i for buzzer", moduleConfig.external_notification.output_buzzer);
                pinMode(moduleConfig.external_notification.output_buzzer, OUTPUT);
                setExternalState(2, false);
                externalTurnedOn[2] = 0;
            } else {
                config.device.buzzer_gpio = config.device.buzzer_gpio ? config.device.buzzer_gpio : PIN_BUZZER;
                // in PWM Mode we force the buzzer pin if it is set
                LOG_INFO("Use Pin %i in PWM mode", config.device.buzzer_gpio);
            }
        }
#ifdef HAS_NCP5623
        if (rgb_found.type == ScanI2C::NCP5623) {
            rgb.begin();
            rgb.setCurrent(10);
        }
#endif
#ifdef HAS_LP5562
        if (rgb_found.type == ScanI2C::LP5562) {
            rgbw.begin();
            rgbw.setCurrent(20);
        }
#endif
#ifdef RGBLED_RED
        pinMode(RGBLED_RED, OUTPUT); // set up the RGB led pins
        pinMode(RGBLED_GREEN, OUTPUT);
        pinMode(RGBLED_BLUE, OUTPUT);
#endif
#ifdef RGBLED_CA
        analogWrite(RGBLED_RED, 255);   // with a common anode type, logic is reversed
        analogWrite(RGBLED_GREEN, 255); // so we want to initialise with lights off
        analogWrite(RGBLED_BLUE, 255);
#endif
#ifdef HAS_NEOPIXEL
        pixels.begin(); // Initialise the pixel(s)
        pixels.clear(); // Set all pixel colors to 'off'
        pixels.setBrightness(moduleConfig.ambient_lighting.current);
#endif
    } else {
        LOG_INFO("External Notification Module Disabled");
        disable();
    }
}

ProcessMessage ExternalNotificationModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (moduleConfig.external_notification.enabled) {
#ifdef T_WATCH_S3
        drv.setWaveform(0, 75);
        drv.setWaveform(1, 56);
        drv.setWaveform(2, 0);
        drv.go();
#endif
        if (!isFromUs(&mp)) {
            const bool sosPacket = isSosAlert(mp);
            // While SOS alarm is sounding, do not let unrelated packets interfere with the active alarm.
            if ((isNagging && currentNagSound == NAG_SOUND_SOS && nagCycleCutoff != UINT32_MAX &&
                 (int32_t)(nagCycleCutoff - millis()) > 0) &&
                !sosPacket) {
                setIntervalFromNow(0);
                return ProcessMessage::CONTINUE;
            }

            // Check if the message contains a bell character. Don't do this loop for every pin, just once.
            auto &p = mp.decoded;
            bool containsBell = false;
            for (size_t i = 0; i < p.payload.size; i++) {
                if (p.payload.bytes[i] == ASCII_BELL) {
                    containsBell = true;
                }
            }

            // Notification outputs are still subject to local mute.
            if (isSilenced) {
                LOG_INFO("External Notification Module muted");
                return ProcessMessage::CONTINUE;
            }

            meshtastic_NodeInfoLite *sender = nodeDB->getMeshNode(mp.from);
            meshtastic_Channel ch = channels.getByIndex(mp.channel ? mp.channel : channels.getPrimaryIndex());

            // If we receive a broadcast message, apply channel mute setting
            // If we receive a direct message and the receipent is us, apply DM mute setting
            // Else we just handle it as not muted.
            const bool directToUs = !isBroadcast(mp.to) && isToUs(&mp);
            bool is_muted = directToUs ? (sender && ((sender->bitfield & NODEINFO_BITFIELD_IS_MUTED_MASK) != 0))
                                       : (ch.settings.has_module_settings && ch.settings.module_settings.is_muted);

            if (moduleConfig.external_notification.alert_bell) {
                if (containsBell) {
                    LOG_INFO("externalNotificationModule - Notification Bell");
                    isNagging = true;
                    currentNagSound = NAG_SOUND_NORMAL;
                    setExternalState(0, true);
                    if (moduleConfig.external_notification.nag_timeout) {
                        nagCycleCutoff = millis() + moduleConfig.external_notification.nag_timeout * 1000;
                    } else {
                        nagCycleCutoff = millis() + moduleConfig.external_notification.output_ms;
                    }
                }
            }

            if (moduleConfig.external_notification.alert_bell_vibra) {
                if (containsBell) {
                    LOG_INFO("externalNotificationModule - Notification Bell (Vibra)");
                    isNagging = true;
                    currentNagSound = NAG_SOUND_NORMAL;
                    setExternalState(1, true);
                    if (moduleConfig.external_notification.nag_timeout) {
                        nagCycleCutoff = millis() + moduleConfig.external_notification.nag_timeout * 1000;
                    } else {
                        nagCycleCutoff = millis() + moduleConfig.external_notification.output_ms;
                    }
                }
            }

            if (moduleConfig.external_notification.alert_bell_buzzer && canBuzz()) {
                if (containsBell) {
                    LOG_INFO("externalNotificationModule - Notification Bell (Buzzer)");
                    isNagging = true;
                    currentNagSound = NAG_SOUND_NORMAL;
                    if (!moduleConfig.external_notification.use_pwm && !moduleConfig.external_notification.use_i2s_as_buzzer) {
                        setExternalState(2, true);
                    } else {
#ifdef HAS_I2S
                        if (moduleConfig.external_notification.use_i2s_as_buzzer) {
                            audioThread->beginRttl(rtttlConfig.ringtone, strlen_P(rtttlConfig.ringtone));
                        } else
#endif
                            if (moduleConfig.external_notification.use_pwm) {
                            rtttl::begin(config.device.buzzer_gpio, rtttlConfig.ringtone);
                        }
                    }
                    if (moduleConfig.external_notification.nag_timeout) {
                        nagCycleCutoff = millis() + moduleConfig.external_notification.nag_timeout * 1000;
                    } else {
                        nagCycleCutoff = millis() + moduleConfig.external_notification.output_ms;
                    }
                }
            }

            if (moduleConfig.external_notification.alert_message && !is_muted) {
                LOG_INFO("externalNotificationModule - Notification Module");
                isNagging = true;
                currentNagSound = NAG_SOUND_NORMAL;
                setExternalState(0, true);
                if (moduleConfig.external_notification.nag_timeout) {
                    nagCycleCutoff = millis() + moduleConfig.external_notification.nag_timeout * 1000;
                } else {
                    nagCycleCutoff = millis() + moduleConfig.external_notification.output_ms;
                }
            }

            if (moduleConfig.external_notification.alert_message_vibra && !is_muted) {
                LOG_INFO("externalNotificationModule - Notification Module (Vibra)");
                isNagging = true;
                currentNagSound = NAG_SOUND_NORMAL;
                setExternalState(1, true);
                if (moduleConfig.external_notification.nag_timeout) {
                    nagCycleCutoff = millis() + moduleConfig.external_notification.nag_timeout * 1000;
                } else {
                    nagCycleCutoff = millis() + moduleConfig.external_notification.output_ms;
                }
            }

            if (moduleConfig.external_notification.alert_message_buzzer && !is_muted) {
                // SOS-ACK DM is handled by TextMessageModule with a distinct chirp; suppress generic message beep here.
                if (isSosAckDmToUs(mp)) {
                    setIntervalFromNow(0);
                    return ProcessMessage::CONTINUE;
                }
                const bool buzzerAllowed = (config.device.buzzer_mode != meshtastic_Config_DeviceConfig_BuzzerMode_DIRECT_MSG_ONLY ||
                                           (!isBroadcast(mp.to) && isToUs(&mp)));
                if (buzzerAllowed) {
                    isNagging = true;
                    const bool sos = sosPacket;
                    currentNagSound = sos ? NAG_SOUND_SOS : NAG_SOUND_NORMAL; // set for entire nag cycle, even when !canBuzz()

                    // Start/refresh SOS repeating state on first SOS (dedup the 2-packet SOS gesture).
                    if (sos) {
                        const uint32_t now = millis();
                        const bool dup = (sosActive && mp.from == sosFrom && (uint32_t)(now - lastSosStartAtMs) < SOS_START_DEDUP_MS);
                        if (!dup) {
                            sosActive = true;
                            sosFrom = mp.from;
                            lastSosStartAtMs = now;
                            sosStartAtMs = now;
                            sosRepeatCount = 0;
                            nextRepeatAtMs = now + SOS_FAST_REPEAT_MS;
                        }
                        if (dup) {
                            // Keep existing alarm playing, but don't start a second time.
                            setIntervalFromNow(0);
                            return ProcessMessage::CONTINUE;
                        }
                    }

                    if (sos && canBuzz()) {
                        // SOS path: distinct pattern; do not play generic message beep for this packet.
                        LOG_INFO("externalNotificationModule - SOS Alert (Buzzer)");
                        if (!moduleConfig.external_notification.use_pwm && !moduleConfig.external_notification.use_i2s_as_buzzer) {
                            setExternalState(2, true);
                            if (moduleConfig.external_notification.nag_timeout) {
                                nagCycleCutoff = millis() + moduleConfig.external_notification.nag_timeout * 1000;
                            } else {
                                uint32_t ms = moduleConfig.external_notification.output_ms ? moduleConfig.external_notification.output_ms * 3 : 3000;
                                nagCycleCutoff = millis() + ms;
                            }
                        } else {
#ifdef HAS_I2S
                            if (moduleConfig.external_notification.use_i2s_as_buzzer) {
                                audioThread->beginRttl(SOS_RINGTONE, strlen(SOS_RINGTONE));
                            } else
#endif
                                if (moduleConfig.external_notification.use_pwm) {
                                rtttl::begin(config.device.buzzer_gpio, SOS_RINGTONE);
                            }
                            if (moduleConfig.external_notification.nag_timeout) {
                                nagCycleCutoff = millis() + moduleConfig.external_notification.nag_timeout * 1000;
                            } else {
                                uint32_t ms = moduleConfig.external_notification.output_ms ? moduleConfig.external_notification.output_ms * 3 : 3000;
                                nagCycleCutoff = millis() + ms;
                            }
                        }
                    } else if (!sos) {
                        // Normal message path: existing generic beep/ringtone. BEL path unchanged above.
                        LOG_INFO("externalNotificationModule - Notification Module (Buzzer)");
#ifdef T_LORA_PAGER
                        if (canBuzz()) {
                            drv.setWaveform(0, 16); // Long buzzer 100%
                            drv.setWaveform(1, 0);  // Pause
                            drv.setWaveform(2, 16);
                            drv.setWaveform(3, 0);
                            drv.setWaveform(4, 16);
                            drv.setWaveform(5, 0);
                            drv.setWaveform(6, 16);
                            drv.setWaveform(7, 0);
                            drv.go();
                        }
#endif
                        if (!moduleConfig.external_notification.use_pwm && !moduleConfig.external_notification.use_i2s_as_buzzer) {
                            setExternalState(2, true);
                        } else {
#ifdef HAS_I2S
                            if (moduleConfig.external_notification.use_i2s_as_buzzer) {
                                audioThread->beginRttl(rtttlConfig.ringtone, strlen_P(rtttlConfig.ringtone));
                            } else
#endif
                                if (moduleConfig.external_notification.use_pwm) {
                                rtttl::begin(config.device.buzzer_gpio, rtttlConfig.ringtone);
                            }
                        }
                        if (moduleConfig.external_notification.nag_timeout) {
                            nagCycleCutoff = millis() + moduleConfig.external_notification.nag_timeout * 1000;
                        } else {
                            nagCycleCutoff = millis() + moduleConfig.external_notification.output_ms;
                        }
                    } else {
                        // sos && !canBuzz(): we set isNagging and currentNagSound but don't play; use same SOS duration as other SOS paths
                        if (moduleConfig.external_notification.nag_timeout) {
                            nagCycleCutoff = millis() + moduleConfig.external_notification.nag_timeout * 1000;
                        } else {
                            uint32_t ms = moduleConfig.external_notification.output_ms ? moduleConfig.external_notification.output_ms * 3 : 3000;
                            nagCycleCutoff = millis() + ms;
                        }
                    }
                } else {
                    // Don't beep if buzzer mode is "direct messages only" and it is no direct message
                    LOG_INFO("Message buzzer was suppressed because buzzer mode DIRECT_MSG_ONLY");
                }
            }
            setIntervalFromNow(0); // run once so we know if we should do something
        }
    } else {
        LOG_INFO("External Notification Module Disabled or muted");
    }

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}

/**
 * @brief An admin message arrived to AdminModule. We are asked whether we want to handle that.
 *
 * @param mp The mesh packet arrived.
 * @param request The AdminMessage request extracted from the packet.
 * @param response The prepared response
 * @return AdminMessageHandleResult HANDLED if message was handled
 *   HANDLED_WITH_RESULT if a result is also prepared.
 */
AdminMessageHandleResult ExternalNotificationModule::handleAdminMessageForModule(const meshtastic_MeshPacket &mp,
                                                                                 meshtastic_AdminMessage *request,
                                                                                 meshtastic_AdminMessage *response)
{
    AdminMessageHandleResult result;

    switch (request->which_payload_variant) {
    case meshtastic_AdminMessage_get_ringtone_request_tag:
        LOG_INFO("Client getting ringtone");
        this->handleGetRingtone(mp, response);
        result = AdminMessageHandleResult::HANDLED_WITH_RESPONSE;
        break;

    case meshtastic_AdminMessage_set_ringtone_message_tag:
        LOG_INFO("Client setting ringtone");
        this->handleSetRingtone(request->set_canned_message_module_messages);
        result = AdminMessageHandleResult::HANDLED;
        break;

    default:
        result = AdminMessageHandleResult::NOT_HANDLED;
    }

    return result;
}

void ExternalNotificationModule::handleGetRingtone(const meshtastic_MeshPacket &req, meshtastic_AdminMessage *response)
{
    LOG_INFO("*** handleGetRingtone");
    if (req.decoded.want_response) {
        response->which_payload_variant = meshtastic_AdminMessage_get_ringtone_response_tag;
        strncpy(response->get_ringtone_response, rtttlConfig.ringtone, sizeof(response->get_ringtone_response));
    } // Don't send anything if not instructed to. Better than asserting.
}

void ExternalNotificationModule::handleSetRingtone(const char *from_msg)
{
    int changed = 0;

    if (*from_msg) {
        changed |= strcmp(rtttlConfig.ringtone, from_msg);
        strncpy(rtttlConfig.ringtone, from_msg, sizeof(rtttlConfig.ringtone));
        LOG_INFO("*** from_msg.text:%s", from_msg);
    }

    if (changed) {
        nodeDB->saveProto(rtttlConfigFile, meshtastic_RTTTLConfig_size, &meshtastic_RTTTLConfig_msg, &rtttlConfig);
    }
}

int ExternalNotificationModule::handleInputEvent(const InputEvent *event)
{
    if (nagCycleCutoff != UINT32_MAX) {
        // Local acknowledge for SOS repeating: button press DURING active SOS alarm stops repeats.
        if (sosActive && isNagging && currentNagSound == NAG_SOUND_SOS && nagCycleCutoff != UINT32_MAX &&
            (int32_t)(nagCycleCutoff - millis()) > 0) {
            sosActive = false;
            sosFrom = 0;
            nextRepeatAtMs = 0;
            lastSosStartAtMs = 0;
        }
        stopNow();
        return 1;
    }
    return 0;
}