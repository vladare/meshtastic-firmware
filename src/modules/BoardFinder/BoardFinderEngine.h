#pragma once

#if defined(TRACKER_T1000_E)

#include "boardfinder_protocol.h"
#include "concurrency/Lock.h"
#include "concurrency/OSThread.h"
#include <stdint.h>

/**
 * Board-role proximity engine: BLE discovery/sync/monitor/separation + GPS tracking.
 * Rider advertising remains in NRF52Bluetooth; this engine only runs when role=BOARD.
 */
class BoardFinderEngine : private concurrency::OSThread
{
  public:
    struct Config {
        boardfinder::DeviceRole role = boardfinder::DeviceRole::NORMAL;
        uint32_t rider_node_id = 0;
        bool board_armed = false;
        bool ble_burst_enabled = false;
    };

    struct StatusSnapshot {
        boardfinder::DeviceRole device_role = boardfinder::DeviceRole::NORMAL;
        uint32_t rider_node_id = 0;
        bool board_armed = false;
        boardfinder::BoardState board_state = boardfinder::BoardState::DISARMED;
        bool synchronized = false;
        uint32_t consecutive_missed_bursts = 0;
        int32_t last_rider_rssi = 0;
        bool has_last_rider_rssi = false;
        uint32_t last_rider_seen_age_seconds = 0;
        bool has_last_rider_seen_age_seconds = false;
        bool gps_tracking_active = false;
        boardfinder::GpsReportStatus last_gps_report_status = boardfinder::GpsReportStatus::NONE;
    };

    BoardFinderEngine();

    void applyConfig(const Config &cfg, bool restartIfArmed);
    boardfinder::ErrorCode arm();
    boardfinder::ErrorCode disarm();
    StatusSnapshot status() const;

    // SoftDevice scan callback (may run outside main thread).
    static void onRiderAdvertisement(uint32_t nodeId, int8_t rssi, uint32_t nowMs);

  protected:
    int32_t runOnce() override;

  private:
    static BoardFinderEngine *instance;
    static concurrency::Lock &obsLock();

    void handleObservationLocked(uint32_t nodeId, int8_t rssi, uint32_t nowMs);
    void transitionTo(boardfinder::BoardState next);
    void startDiscoveryScan();
    void stopScan();
    void openScanWindow(uint32_t nowMs);
    void closeScanWindow(uint32_t nowMs);
    void noteBurst(uint32_t burstStartMs, int8_t rssi);
    void onSyncComplete();
    void onMissedWindow();
    void enterSeparated();
    void recoverFromSeparated(uint32_t burstStartMs, int8_t rssi);
    void maybeReportGps(uint32_t nowMs);
    void stopGpsTracking();
    void resetAcquisitionSoundFlags();
    uint32_t expectedBurstStart(uint32_t nowMs) const;

    Config cfg{};
    boardfinder::BoardState state = boardfinder::BoardState::DISARMED;
    bool synchronized = false;
    uint32_t consecutiveMissed = 0;
    uint32_t armStartedMs = 0;
    bool notFoundSoundPlayed = false;
    uint32_t firstBurstMs = 0;
    uint32_t phaseAnchorMs = 0;
    uint32_t lastBurstStartMs = 0;
    uint32_t currentBurstStartMs = 0;
    bool inBurstObservation = false;
    bool windowOpen = false;
    bool windowSawRider = false;
    uint32_t windowStartMs = 0;
    uint32_t windowEndMs = 0;
    uint32_t nextWindowOpenMs = 0;
    uint32_t reacquireUntilMs = 0;
    uint32_t periodsSinceReacquire = 0;

    int8_t lastRssi = 0;
    bool hasRssi = false;
    uint32_t lastSeenMs = 0;
    bool hasSeen = false;

    bool gpsTrackingActive = false;
    uint32_t lastGpsAttemptMs = 0;
    boardfinder::GpsReportStatus lastGpsStatus = boardfinder::GpsReportStatus::NONE;

    // Latest observation posted by scan callback.
    uint32_t pendingNodeId = 0;
    int8_t pendingRssi = 0;
    uint32_t pendingMs = 0;
    bool pendingValid = false;
};

extern BoardFinderEngine *boardFinderEngine;

#endif
