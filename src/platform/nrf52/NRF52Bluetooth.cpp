#include "NRF52Bluetooth.h"
#include "BLEDfuSecure.h"
#include "BluetoothCommon.h"
#include "HardwareRNG.h"
#include "PowerFSM.h"
#include "configuration.h"
#include "main.h"
#include "mesh/PhoneAPI.h"
#include "mesh/mesh-pb-constants.h"
#if defined(TRACKER_T1000_E)
#include "T1000eBleBurst.h"
#include "concurrency/LockGuard.h"
#include "concurrency/OSThread.h"
#include "mesh/NodeDB.h"
#endif
#include <bluefruit.h>
#include <utility/bonding.h>
static BLEService meshBleService = BLEService(BLEUuid(MESH_SERVICE_UUID_16));
static BLECharacteristic fromNum = BLECharacteristic(BLEUuid(FROMNUM_UUID_16));
static BLECharacteristic fromRadio = BLECharacteristic(BLEUuid(FROMRADIO_UUID_16));
static BLECharacteristic toRadio = BLECharacteristic(BLEUuid(TORADIO_UUID_16));
static BLECharacteristic logRadio = BLECharacteristic(BLEUuid(LOGRADIO_UUID_16));

static BLEDis bledis; // DIS (Device Information Service) helper class instance
static BLEBas blebas; // BAS (Battery Service) helper class instance
#ifndef BLE_DFU_SECURE
static BLEDfu bledfu; // DFU software update helper service
#else
static BLEDfuSecure bledfusecure;                                             // DFU software update helper service
#endif

// This scratch buffer is used for various bluetooth reads/writes - but it is safe because only one bt operation can be in
// process at once
// static uint8_t trBytes[_max(_max(_max(_max(ToRadio_size, RadioConfig_size), User_size), MyNodeInfo_size), FromRadio_size)];
static uint8_t fromRadioBytes[meshtastic_FromRadio_size];
static uint8_t toRadioBytes[meshtastic_ToRadio_size];

// Last ToRadio value received from the phone
static uint8_t lastToRadio[MAX_TO_FROM_RADIO_SIZE];

static uint16_t connectionHandle;
static bool passkeyShowing;

#if defined(TRACKER_T1000_E)
static void t1000eBurstInit();
static void t1000eBurstOnConnect();
static void t1000eBurstOnDisconnect();
static void t1000eBurstSuspend();
static void t1000eResumeConnectableAdvertising();
#endif

class BluetoothPhoneAPI : public PhoneAPI
{
    /**
     * Subclasses can use this as a hook to provide custom notifications for their transport (i.e. bluetooth notifies)
     */
    virtual void onNowHasData(uint32_t fromRadioNum) override
    {
        PhoneAPI::onNowHasData(fromRadioNum);

        LOG_INFO("BLE notify fromNum");
        fromNum.notify32(fromRadioNum);
    }

    /// Check the current underlying physical link to see if the client is currently connected
    virtual bool checkIsConnected() override { return Bluefruit.connected(connectionHandle); }

  public:
    BluetoothPhoneAPI() { api_type = TYPE_BLE; }
};

static BluetoothPhoneAPI *bluetoothPhoneAPI;

void onConnect(uint16_t conn_handle)
{
    // Get the reference to current connection
    BLEConnection *connection = Bluefruit.Connection(conn_handle);
    connectionHandle = conn_handle;
    char central_name[32] = {0};
    connection->getPeerName(central_name, sizeof(central_name));
    LOG_INFO("BLE Connected to %s", central_name);

    // Notify UI (or any other interested firmware components)
    meshtastic::BluetoothStatus newStatus(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
    bluetoothStatus->updateStatus(&newStatus);
#if defined(TRACKER_T1000_E)
    t1000eBurstOnConnect();
#endif
}
/**
 * Callback invoked when a connection is dropped
 * @param conn_handle connection where this event happens
 * @param reason is a BLE_HCI_STATUS_CODE which can be found in ble_hci.h
 */
void onDisconnect(uint16_t conn_handle, uint8_t reason)
{
    LOG_INFO("BLE Disconnected, reason = 0x%x", reason);
    if (bluetoothPhoneAPI) {
        bluetoothPhoneAPI->close();
    }

    // Clear the last ToRadio packet buffer to avoid rejecting first packet from new connection
    memset(lastToRadio, 0, sizeof(lastToRadio));

    // Notify UI (or any other interested firmware components)
    meshtastic::BluetoothStatus newStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
    bluetoothStatus->updateStatus(&newStatus);

#if HAS_SCREEN
    // If a pairing prompt is active, make sure we dismiss it on disconnect/cancel/failure paths.
    if (passkeyShowing) {
        passkeyShowing = false;
        if (screen) {
            screen->endAlert();
        }
    }
#endif
#if defined(TRACKER_T1000_E)
    t1000eBurstOnDisconnect();
#endif
}
void onCccd(uint16_t conn_hdl, BLECharacteristic *chr, uint16_t cccd_value)
{
    // Display the raw request packet
    LOG_INFO("CCCD Updated: %u", cccd_value);
    // Check the characteristic this CCCD update is associated with in case
    // this handler is used for multiple CCCD records.

    // According to the GATT spec: cccd value = 0x0001 means notifications are enabled
    // and cccd value = 0x0002 means indications are enabled

    if (chr->uuid == fromNum.uuid || chr->uuid == logRadio.uuid) {
        auto result = cccd_value == 2 ? chr->indicateEnabled(conn_hdl) : chr->notifyEnabled(conn_hdl);
        if (result) {
            LOG_INFO("Notify/Indicate enabled");
        } else {
            LOG_INFO("Notify/Indicate disabled");
        }
    }
}
void startAdv(void)
{
    // Advertising packet
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    // IncludeService UUID
    // Bluefruit.ScanResponse.addService(meshBleService);
    Bluefruit.ScanResponse.addTxPower();
    Bluefruit.ScanResponse.addName();
    // Include Name
    // Bluefruit.Advertising.addName();
    Bluefruit.Advertising.addService(meshBleService);
    /* Start Advertising
     * - Enable auto advertising if disconnected
     * - Interval:  fast mode = 20 ms, slow mode = 417,5 ms
     * - Timeout for fast mode is 30 seconds
     * - Start(timeout) with timeout = 0 will advertise forever (until connected)
     *
     * For recommended advertising interval
     * https://developer.apple.com/library/content/qa/qa1931/_index.html
     */
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 668); // in unit of 0.625 ms
    Bluefruit.Advertising.setFastTimeout(30);   // number of seconds in fast mode
    Bluefruit.Advertising.start(0); // 0 = Don't stop advertising after n seconds.  FIXME, we should stop advertising after X
}
// Just ack that the caller is allowed to read
static void authorizeRead(uint16_t conn_hdl)
{
    ble_gatts_rw_authorize_reply_params_t reply = {.type = BLE_GATTS_AUTHORIZE_TYPE_READ};
    reply.params.write.gatt_status = BLE_GATT_STATUS_SUCCESS;
    sd_ble_gatts_rw_authorize_reply(conn_hdl, &reply);
}
/**
 * client is starting read, pull the bytes from our API class
 */
void onFromRadioAuthorize(uint16_t conn_hdl, BLECharacteristic *chr, ble_gatts_evt_read_t *request)
{
    if (request->offset == 0) {
        // If the read is long, we will get multiple authorize invocations - we only populate data on the first
        size_t numBytes = bluetoothPhoneAPI->getFromRadio(fromRadioBytes);
        // Someone is going to read our value as soon as this callback returns.  So fill it with the next message in the queue
        // or make empty if the queue is empty
        fromRadio.write(fromRadioBytes, numBytes);
    } else {
        // LOG_INFO("Ignore successor read");
    }
    authorizeRead(conn_hdl);
}

void onToRadioWrite(uint16_t conn_hdl, BLECharacteristic *chr, uint8_t *data, uint16_t len)
{
    LOG_INFO("toRadioWriteCb data %p, len %u", data, len);
    if (memcmp(lastToRadio, data, len) != 0) {
        LOG_DEBUG("New ToRadio packet");
        memcpy(lastToRadio, data, len);
        bluetoothPhoneAPI->handleToRadio(data, len);
    } else {
        LOG_DEBUG("Drop dup ToRadio packet we just saw");
    }
}

void setupMeshService(void)
{
    bluetoothPhoneAPI = new BluetoothPhoneAPI();
    meshBleService.begin();
    // Note: You must call .begin() on the BLEService before calling .begin() on
    // any characteristic(s) within that service definition.. Calling .begin() on
    // a BLECharacteristic will cause it to be added to the last BLEService that
    // was 'begin()'ed!
    auto secMode =
        config.bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN ? SECMODE_OPEN : SECMODE_ENC_NO_MITM;
    fromNum.setProperties(CHR_PROPS_NOTIFY | CHR_PROPS_READ);
    fromNum.setPermission(secMode, SECMODE_NO_ACCESS); // FIXME, secure this!!!
    fromNum.setFixedLen(
        0); // Variable len (either 0 or 4)  FIXME consider changing protocol so it is fixed 4 byte len, where 0 means empty
    fromNum.setMaxLen(4);
    fromNum.setCccdWriteCallback(onCccd); // Optionally capture CCCD updates
    // We don't yet need to hook the fromNum auth callback
    // fromNum.setReadAuthorizeCallback(fromNumAuthorizeCb);
    fromNum.write32(0); // Provide default fromNum of 0
    fromNum.begin();

    fromRadio.setProperties(CHR_PROPS_READ);
    fromRadio.setPermission(secMode, SECMODE_NO_ACCESS);
    fromRadio.setMaxLen(sizeof(fromRadioBytes));
    fromRadio.setReadAuthorizeCallback(
        onFromRadioAuthorize,
        false); // We don't call this callback via the adafruit queue, because we can safely run in the BLE context
    fromRadio.setBuffer(fromRadioBytes, sizeof(fromRadioBytes)); // we preallocate our fromradio buffer so we won't waste space
    // for two copies
    fromRadio.begin();

    toRadio.setProperties(CHR_PROPS_WRITE);
    toRadio.setPermission(secMode, secMode); // FIXME secure this!
    toRadio.setFixedLen(0);
    toRadio.setMaxLen(512);
    toRadio.setBuffer(toRadioBytes, sizeof(toRadioBytes));
    // We don't call this callback via the adafruit queue, because we can safely run in the BLE context
    toRadio.setWriteCallback(onToRadioWrite, false);
    toRadio.begin();

    logRadio.setProperties(CHR_PROPS_INDICATE | CHR_PROPS_NOTIFY | CHR_PROPS_READ);
    logRadio.setPermission(secMode, SECMODE_NO_ACCESS);
    logRadio.setMaxLen(512);
    logRadio.setCccdWriteCallback(onCccd);
    logRadio.write32(0);
    logRadio.begin();
}
static uint32_t configuredPasskey;
void NRF52Bluetooth::shutdown()
{
#if defined(TRACKER_T1000_E)
    // Stop experimental bursts before the link is torn down. Do not restart advertising afterwards.
    t1000eBurstSuspend();
#endif
    // Shutdown bluetooth for minimum power draw
    LOG_INFO("Disable NRF52 bluetooth");
    Bluefruit.Security.setPairPasskeyCallback(NRF52Bluetooth::onUnwantedPairing); // Actively refuse (during factory reset)
    disconnect();
    Bluefruit.Advertising.stop();
}
void NRF52Bluetooth::startDisabled()
{
    // Setup Bluetooth
    nrf52Bluetooth->setup();
    // Shutdown bluetooth for minimum power draw
    Bluefruit.Advertising.stop();
    Bluefruit.setTxPower(-40); // Minimum power
    LOG_INFO("Disable NRF52 Bluetooth. (Workaround: tx power min, advertise stopped)");
}
bool NRF52Bluetooth::isConnected()
{
    return Bluefruit.connected(connectionHandle);
}
int NRF52Bluetooth::getRssi()
{
    return 0; // FIXME figure out where to source this
}

// Valid BLE TX power levels as per nRF52840 Product Specification are: "-20 to +8 dBm TX power, configurable in 4 dB steps".
// See https://docs.nordicsemi.com/bundle/ps_nrf52840/page/keyfeatures_html5.html
#define VALID_BLE_TX_POWER(x)                                                                                                    \
    ((x) == -20 || (x) == -16 || (x) == -12 || (x) == -8 || (x) == -4 || (x) == 0 || (x) == 4 || (x) == 8)

void NRF52Bluetooth::setup()
{
    // Initialise the Bluefruit module
    LOG_INFO("Init the Bluefruit nRF52 module");
    Bluefruit.autoConnLed(false);
    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
    Bluefruit.begin();
    // Clear existing data.
    Bluefruit.Advertising.stop();
    Bluefruit.Advertising.clearData();
    Bluefruit.ScanResponse.clearData();
#if defined(NRF52_BLE_TX_POWER) && VALID_BLE_TX_POWER(NRF52_BLE_TX_POWER)
    Bluefruit.setTxPower(NRF52_BLE_TX_POWER);
#endif
    if (config.bluetooth.mode != meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN) {
        if (config.bluetooth.mode == meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN) {
            configuredPasskey = config.bluetooth.fixed_pin;
        } else {
            uint32_t hwrand = 0;
            HardwareRNG::fill(reinterpret_cast<uint8_t *>(&hwrand), sizeof(hwrand));
            configuredPasskey = hwrand % 900000u + 100000u;
        }
        auto pinString = std::to_string(configuredPasskey);
        LOG_INFO("Bluetooth pin set to '%i'", configuredPasskey);
        Bluefruit.Security.setPIN(pinString.c_str());
        Bluefruit.Security.setIOCaps(true, false, false);
        Bluefruit.Security.setPairPasskeyCallback(NRF52Bluetooth::onPairingPasskey);
        Bluefruit.Security.setPairCompleteCallback(NRF52Bluetooth::onPairingCompleted);
        Bluefruit.Security.setSecuredCallback(NRF52Bluetooth::onConnectionSecured);
        meshBleService.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
    } else {
        Bluefruit.Security.setIOCaps(false, false, false);
        meshBleService.setPermission(SECMODE_OPEN, SECMODE_OPEN);
    }
    // Set the advertised device name (keep it short!)
    Bluefruit.setName(getDeviceName());
    // Set the connect/disconnect callback handlers
    Bluefruit.Periph.setConnectCallback(onConnect);
    Bluefruit.Periph.setDisconnectCallback(onDisconnect);

    // Do not change Slave Latency to value other than 0 !!!
    // There is probably a bug in SoftDevice + certain Apple iOS versions being
    // brain damaged causing connectivity problems.

    // On one side it seems SoftDevice is using SlaveLatency value even
    // if connection parameter negotation failed and phone sees it as connectivity errors.

    // On the other hand Apple can randomly refuse any parameter negotiation and shutdown connection
    // even if you meet Apple Developer Guidelines for BLE devices. Because f* you, that's why.

    // While this API call sets preferred connection parameters (PPCP) - many phones ignore it (yeah) and it seems SoftDevice
    // will try to renegotiate connection parameters based on those values after phone connection.
    // So those are relatively safe values so Apple braindead firmware won't get angry and at least we may try
    // to negotiate some longer connection interval to save battery.

    // See https://github.com/meshtastic/firmware/pull/8858 for measurements.  We are dealing with microamp savings anyway so not
    // worth dying on a hill here.

    Bluefruit.Periph.setConnSlaveLatency(0);
    // 1.25 ms units - so min, max is 15, 100 ms range.
    Bluefruit.Periph.setConnInterval(12, 80);

#ifndef BLE_DFU_SECURE
    bledfu.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
    bledfu.begin(); // Install the DFU helper
#else
    bledfusecure.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM); // add by WayenWeng
    bledfusecure.begin();                                                     // Install the DFU helper
#endif
    // Configure and Start the Device Information Service
    LOG_INFO("Init the Device Information Service");
    bledis.setModel(optstr(HW_VERSION));
    bledis.setFirmwareRev(optstr(APP_VERSION));
    bledis.begin();
    // Start the BLE Battery Service and set it to 100%
    LOG_INFO("Init the Battery Service");
    blebas.begin();
    blebas.write(0); // Unknown battery level for now
    // Setup the Heart Rate Monitor service using
    // BLEService and BLECharacteristic classes
    LOG_INFO("Init the Mesh bluetooth service");
    setupMeshService();
    // Setup the advertising packet(s)
    LOG_INFO("Set up the advertising payload(s)");
    startAdv();
    LOG_INFO("Advertise");
#if defined(TRACKER_T1000_E)
    t1000eBurstInit();
#endif
}
void NRF52Bluetooth::resumeAdvertising()
{
#if defined(TRACKER_T1000_E)
    t1000eResumeConnectableAdvertising();
#else
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 668); // in unit of 0.625 ms
    Bluefruit.Advertising.setFastTimeout(30);   // number of seconds in fast mode
    Bluefruit.Advertising.start(0);
#endif
}
/// Given a level between 0-100, update the BLE attribute
void updateBatteryLevel(uint8_t level)
{
    blebas.write(level);
}
void NRF52Bluetooth::clearBonds()
{
    LOG_INFO("Clear bluetooth bonds!");
    bond_print_list(BLE_GAP_ROLE_PERIPH);
    bond_print_list(BLE_GAP_ROLE_CENTRAL);
    Bluefruit.Periph.clearBonds();
    Bluefruit.Central.clearBonds();
}
void NRF52Bluetooth::onConnectionSecured(uint16_t conn_handle)
{
    LOG_INFO("BLE connection secured");
}
bool NRF52Bluetooth::onPairingPasskey(uint16_t conn_handle, uint8_t const passkey[6], bool match_request)
{
    char passkey1[4] = {passkey[0], passkey[1], passkey[2], '\0'};
    char passkey2[4] = {passkey[3], passkey[4], passkey[5], '\0'};
    LOG_INFO("BLE pair process started with passkey %s %s", passkey1, passkey2);
    powerFSM.trigger(EVENT_BLUETOOTH_PAIR);

    // Get passkey as string
    // Note: possible leading zeros
    std::string textkey;
    for (uint8_t i = 0; i < 6; i++)
        textkey += (char)passkey[i];

    // Notify UI (or other components) of pairing event and passkey
    meshtastic::BluetoothStatus newStatus(textkey);
    bluetoothStatus->updateStatus(&newStatus);

#if HAS_SCREEN &&                                                                                                                \
    !defined(MESHTASTIC_EXCLUDE_SCREEN) // Todo: migrate this display code back into Screen class, and observe bluetoothStatus
    if (screen) {
        screen->startAlert([](OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) -> void {
            char btPIN[16] = "888888";
            snprintf(btPIN, sizeof(btPIN), "%06u", configuredPasskey);
            int x_offset = display->width() / 2;
            int y_offset = display->height() <= 80 ? 0 : 12;
            display->setTextAlignment(TEXT_ALIGN_CENTER);
            display->setFont(FONT_MEDIUM);
            display->drawString(x_offset + x, y_offset + y, "Bluetooth");

            display->setFont(FONT_SMALL);
            y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_MEDIUM - 4 : y_offset + FONT_HEIGHT_MEDIUM + 5;
            display->drawString(x_offset + x, y_offset + y, "Enter this code");

            display->setFont(FONT_LARGE);
            String displayPin(btPIN);
            String pin = displayPin.substring(0, 3) + " " + displayPin.substring(3, 6);
            y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_SMALL - 5 : y_offset + FONT_HEIGHT_SMALL + 5;
            display->drawString(x_offset + x, y_offset + y, pin);

            display->setFont(FONT_SMALL);
            String deviceName = "Name: ";
            deviceName.concat(getDeviceName());
            y_offset = display->height() == 64 ? y_offset + FONT_HEIGHT_LARGE - 6 : y_offset + FONT_HEIGHT_LARGE + 5;
            display->drawString(x_offset + x, y_offset + y, deviceName);
        });
    }
#endif
    passkeyShowing = true;

    if (match_request) {
        uint32_t start_time = millis();
        while (millis() < start_time + 30000) {
            if (!Bluefruit.connected(conn_handle))
                break;
        }
    }
    LOG_INFO("BLE passkey pair: match_request=%i", match_request);
    return true;
}

// Actively refuse new BLE pairings
// After clearing bonds (at factory reset), clients seem initially able to attempt to re-pair, even with advertising disabled.
// On NRF52Bluetooth::shutdown, we change the pairing callback to this method, to aggressively refuse any connection attempts.
bool NRF52Bluetooth::onUnwantedPairing(uint16_t conn_handle, uint8_t const passkey[6], bool match_request)
{
    NRF52Bluetooth::disconnect();
    return false;
}

// Disconnect any BLE connections
void NRF52Bluetooth::disconnect()
{
    uint8_t connection_num = Bluefruit.connected();
    if (connection_num) {
        // Close all connections. We're only expecting one.
        for (uint8_t i = 0; i < connection_num; i++)
            Bluefruit.disconnect(i);

        // Wait for disconnection
        while (Bluefruit.connected())
            yield();

        LOG_INFO("Ended BLE connection");
    }
}

void NRF52Bluetooth::onPairingCompleted(uint16_t conn_handle, uint8_t auth_status)
{
    if (auth_status == BLE_GAP_SEC_STATUS_SUCCESS) {
        LOG_INFO("BLE pair success");
        meshtastic::BluetoothStatus newConnectedStatus(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
        bluetoothStatus->updateStatus(&newConnectedStatus);
    } else {
        LOG_INFO("BLE pair failed");
        // Notify UI (or any other interested firmware components)
        meshtastic::BluetoothStatus newDisconnectedStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
        bluetoothStatus->updateStatus(&newDisconnectedStatus);
    }

    // Todo: migrate this display code back into Screen class, and observe bluetoothStatus
    passkeyShowing = false;
    if (screen) {
        screen->endAlert();
    }
}

void NRF52Bluetooth::sendLog(const uint8_t *logMessage, size_t length)
{
    if (!isConnected() || length > 512)
        return;
    if (logRadio.indicateEnabled())
        logRadio.indicate(logMessage, (uint16_t)length);
    else
        logRadio.notify(logMessage, (uint16_t)length);
}

#if defined(TRACKER_T1000_E)
// Experimental Board Finder beacon for the Rider T1000-E only.
// While Android is disconnected, startAdv()/resumeAdvertising() keep the stock connectable packet.
// While Android is connected AND ble_burst_enabled is true, this thread reuses the single S140
// advertising set for a 3 s non-connectable burst every 60 s (start-to-start), after an initial
// 15 s delay so Meshtastic GATT sync can finish. SoftDevice is driven directly because
// Bluefruit.Advertising.start() uses _fast_timeout and always attaches a scan response.
//
// Ownership: while connected, restartOnDisconnect(false). The deferred onDisconnect path
// exclusively restores connectable advertising.

constexpr uint32_t BLE_BURST_PERIOD_MS = 60000;
constexpr uint32_t BLE_BURST_DURATION_MS = 3000;
constexpr uint32_t BLE_BURST_INITIAL_DELAY_MS = 15000;
constexpr uint32_t BLE_ADV_INTERVAL_MS = 100;
constexpr uint16_t BLE_ADV_INTERVAL_UNITS = static_cast<uint16_t>((BLE_ADV_INTERVAL_MS * 1000U) / 625U);
static_assert(BLE_ADV_INTERVAL_UNITS == 160, "100 ms must be 160 advertising-interval units");

enum class BurstPhase : uint8_t { Idle, Bursting };

static concurrency::Lock &burstLock()
{
    static concurrency::Lock lock;
    return lock;
}

static bool bleBurstEnabled = false;
static bool phoneConnected = false;
static bool restoreSuppressed = false;
static BurstPhase phase = BurstPhase::Idle;
static uint8_t advHandle = 0;
static uint32_t nextBurstAtMs = 0;
static uint32_t burstStartedAtMs = 0;
static uint8_t burstPayload[31];
static uint8_t burstPayloadLen = 0;

class T1000eBleBurstThread : public concurrency::OSThread
{
  public:
    T1000eBleBurstThread() : concurrency::OSThread("T1000EBurst", 0) {}
    void wake()
    {
        enabled = true;
        setIntervalFromNow(0);
    }
    int32_t runOnce() override;
};

static T1000eBleBurstThread *burstThread = nullptr;

static int32_t delayUntil(uint32_t targetMs)
{
    int32_t remaining = static_cast<int32_t>(targetMs - millis());
    return remaining > 0 ? remaining : 0;
}

static void buildBurstPayload(uint32_t nodeId)
{
    burstPayload[0] = 2;
    burstPayload[1] = BLE_GAP_AD_TYPE_FLAGS;
    burstPayload[2] = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;
    constexpr uint8_t meshUuidLen = 16;
    burstPayload[3] = 1 + meshUuidLen + sizeof(nodeId);
    burstPayload[4] = BLE_GAP_AD_TYPE_SERVICE_DATA_128BIT_UUID;
    memcpy(&burstPayload[5], MESH_SERVICE_UUID_16, meshUuidLen);
    burstPayload[21] = static_cast<uint8_t>(nodeId);
    burstPayload[22] = static_cast<uint8_t>(nodeId >> 8);
    burstPayload[23] = static_cast<uint8_t>(nodeId >> 16);
    burstPayload[24] = static_cast<uint8_t>(nodeId >> 24);
    burstPayloadLen = 25;
}

static uint32_t stopAdvertisingSet()
{
    uint32_t err = sd_ble_gap_adv_stop(advHandle);
    if (err != NRF_SUCCESS && err != NRF_ERROR_INVALID_STATE)
        LOG_ERROR("[BLE-BURST] Start failed: sd_ble_gap_adv_stop returned 0x%lx", static_cast<unsigned long>(err));
    return err;
}

static uint32_t startNonConnectableBurst(uint32_t nodeId)
{
    static ble_gap_adv_data_t advData;
    memset(&advData, 0, sizeof(advData));
    buildBurstPayload(nodeId);
    advData.adv_data.p_data = burstPayload;
    advData.adv_data.len = burstPayloadLen;

    ble_gap_adv_params_t params;
    memset(&params, 0, sizeof(params));
    params.properties.type = BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
    params.p_peer_addr = nullptr;
    params.interval = BLE_ADV_INTERVAL_UNITS;
    params.duration = BLE_GAP_ADV_TIMEOUT_GENERAL_UNLIMITED;
    params.max_adv_evts = 0;
    params.filter_policy = BLE_GAP_ADV_FP_ANY;
    params.primary_phy = BLE_GAP_PHY_1MBPS;
    params.secondary_phy = BLE_GAP_PHY_1MBPS;

    uint8_t handle = advHandle;
    uint32_t err = sd_ble_gap_adv_set_configure(&handle, &advData, &params);
    if (err == NRF_ERROR_INVALID_STATE) {
        stopAdvertisingSet();
        handle = advHandle;
        err = sd_ble_gap_adv_set_configure(&handle, &advData, &params);
    }
    if (err != NRF_SUCCESS)
        return err;
    advHandle = handle;

    uint32_t powerErr = sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_ADV, advHandle, Bluefruit.getTxPower());
    if (powerErr != NRF_SUCCESS)
        LOG_ERROR("[BLE-BURST] Start failed: sd_ble_gap_tx_power_set returned 0x%lx", static_cast<unsigned long>(powerErr));

    return sd_ble_gap_adv_start(advHandle, BLE_CONN_CFG_TAG_DEFAULT);
}

static bool restoreConnectableLocked()
{
    stopAdvertisingSet();
    Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED);
    Bluefruit.Advertising.setInterval(32, 668);
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.restartOnDisconnect(false);
    if (Bluefruit.Advertising.start(0)) {
        Bluefruit.Advertising.restartOnDisconnect(true);
        return true;
    }
    stopAdvertisingSet();
    if (Bluefruit.Advertising.start(0)) {
        Bluefruit.Advertising.restartOnDisconnect(true);
        return true;
    }
    return false;
}

static void stopBurstLocked()
{
    uint32_t durationMs = millis() - burstStartedAtMs;
    stopAdvertisingSet();
    phase = BurstPhase::Idle;
    LOG_INFO("[BLE-BURST] Stop t=%u dur=%u", millis(), durationMs);
}

static void armBurstScheduleLocked()
{
    nextBurstAtMs = millis() + BLE_BURST_INITIAL_DELAY_MS;
    phase = BurstPhase::Idle;
    if (burstThread != nullptr)
        burstThread->wake();
}

static void cancelBurstScheduleLocked()
{
    if (phase == BurstPhase::Bursting)
        stopBurstLocked();
    else
        phase = BurstPhase::Idle;
    if (burstThread != nullptr)
        burstThread->disable();
}

int32_t T1000eBleBurstThread::runOnce()
{
    concurrency::LockGuard guard(&burstLock());

    if (restoreSuppressed || !config.bluetooth.enabled || !phoneConnected || !bleBurstEnabled) {
        if (phase == BurstPhase::Bursting)
            stopBurstLocked();
        return disable();
    }

    uint32_t now = millis();
    if (phase == BurstPhase::Bursting) {
        stopBurstLocked();
        return delayUntil(nextBurstAtMs);
    }

    if (delayUntil(nextBurstAtMs) > 0)
        return delayUntil(nextBurstAtMs);

    if (nodeDB == nullptr) {
        LOG_ERROR("[BLE-BURST] Start failed: node database is not ready");
        nextBurstAtMs = now + BLE_BURST_PERIOD_MS;
        return static_cast<int32_t>(BLE_BURST_PERIOD_MS);
    }

    uint32_t nodeId = nodeDB->getNodeNum();
    uint32_t err = startNonConnectableBurst(nodeId);
    if (err != NRF_SUCCESS) {
        LOG_ERROR("[BLE-BURST] Start failed: 0x%lx", static_cast<unsigned long>(err));
        nextBurstAtMs = now + BLE_BURST_PERIOD_MS;
        return static_cast<int32_t>(BLE_BURST_PERIOD_MS);
    }

    phase = BurstPhase::Bursting;
    burstStartedAtMs = millis();
    nextBurstAtMs = burstStartedAtMs + BLE_BURST_PERIOD_MS;
    LOG_INFO("[BLE-BURST] Start t=%u node=0x%08x interval=%ums", burstStartedAtMs, nodeId, BLE_ADV_INTERVAL_MS);
    return static_cast<int32_t>(BLE_BURST_DURATION_MS);
}

static void t1000eBurstInit()
{
    burstLock();
    if (burstThread == nullptr)
        burstThread = new T1000eBleBurstThread();
}

static void t1000eBurstOnConnect()
{
    concurrency::LockGuard guard(&burstLock());
    if (restoreSuppressed || !config.bluetooth.enabled || burstThread == nullptr)
        return;
    Bluefruit.Advertising.restartOnDisconnect(false);
    phoneConnected = true;
    LOG_INFO("[BLE-BURST] Connected t=%u enabled=%d", millis(), bleBurstEnabled ? 1 : 0);
    if (bleBurstEnabled)
        armBurstScheduleLocked();
    else
        cancelBurstScheduleLocked();
}

static void t1000eBurstOnDisconnect()
{
    concurrency::LockGuard guard(&burstLock());
    phoneConnected = false;
    LOG_INFO("[BLE-BURST] Disconnected t=%u", millis());

    cancelBurstScheduleLocked();

    if (restoreSuppressed || !config.bluetooth.enabled) {
        Bluefruit.Advertising.restartOnDisconnect(false);
        return;
    }

    if (restoreConnectableLocked())
        LOG_INFO("[BLE-BURST] Connectable advertising restored t=%u", millis());
    else
        LOG_ERROR("[BLE-BURST] Start failed: connectable restart returned false");
}

static void t1000eBurstSuspend()
{
    concurrency::LockGuard guard(&burstLock());
    restoreSuppressed = true;
    phoneConnected = false;
    Bluefruit.Advertising.restartOnDisconnect(false);
    cancelBurstScheduleLocked();
}

static void t1000eResumeConnectableAdvertising()
{
    concurrency::LockGuard guard(&burstLock());
    restoreSuppressed = false;
    if (phoneConnected) {
        Bluefruit.Advertising.restartOnDisconnect(false);
        return;
    }
    if (!restoreConnectableLocked())
        LOG_ERROR("[BLE-BURST] Start failed: connectable restart returned false");
}

void t1000eBurstSetEnabled(bool enabled)
{
    concurrency::LockGuard guard(&burstLock());
    bleBurstEnabled = enabled;
    LOG_INFO("[BLE-BURST] Enabled=%d", enabled ? 1 : 0);
}

bool t1000eBurstIsEnabled()
{
    concurrency::LockGuard guard(&burstLock());
    return bleBurstEnabled;
}

void t1000eBurstReapply()
{
    concurrency::LockGuard guard(&burstLock());
    if (restoreSuppressed || !config.bluetooth.enabled || !phoneConnected || burstThread == nullptr) {
        cancelBurstScheduleLocked();
        return;
    }
    Bluefruit.Advertising.restartOnDisconnect(false);
    if (bleBurstEnabled)
        armBurstScheduleLocked();
    else
        cancelBurstScheduleLocked();
}
#endif

