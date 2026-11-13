#if defined(TRACKER_T1000_E)

#include "BoardFinderEngine.h"
#include "GPS.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "boardfinder_adv.h"
#include "boardfinder_sound.h"
#include "configuration.h"
#include "main.h"
#include "modules/PositionModule.h"
#include "platform/nrf52/T1000eBoardScan.h"
#include "platform/nrf52/T1000eBleBurst.h"
#include <Arduino.h>

BoardFinderEngine *boardFinderEngine = nullptr;
BoardFinderEngine *BoardFinderEngine::instance = nullptr;

namespace
{
constexpr uint32_t kBurstPeriodMs = 60000;
constexpr uint32_t kBurstDurationMs = 3000;
constexpr uint32_t kScanPreMs = 3000;
constexpr uint32_t kScanPostMs = 3000;
constexpr uint32_t kScanWindowMs = kScanPreMs + kBurstDurationMs + kScanPostMs; // 9 s
constexpr uint32_t kBurstGroupMs = 4000;
// Accept second sync burst from 45 s .. 90 s after the first (covers phase skew + late windows).
constexpr uint32_t kPeriodMinMs = 45000;
constexpr uint32_t kPeriodMaxMs = 90000;
constexpr uint32_t kSyncTimeoutMs = 150000;
constexpr uint32_t kGpsReportIntervalMs = 60000;
constexpr uint32_t kReacquireEveryN = 3;
constexpr uint32_t kReacquireScanMs = 20000;
constexpr uint32_t kDiscoveryNotFoundMs = 60000;
} // namespace

concurrency::Lock &BoardFinderEngine::obsLock()
{
    static concurrency::Lock lock;
    return lock;
}

BoardFinderEngine::BoardFinderEngine() : concurrency::OSThread("BFEngine")
{
    instance = this;
    t1000eBoardScanSetCallback(&BoardFinderEngine::onRiderAdvertisement);
    disable();
}

void BoardFinderEngine::onRiderAdvertisement(uint32_t nodeId, int8_t rssi, uint32_t nowMs)
{
    if (instance == nullptr)
        return;
    concurrency::LockGuard guard(&obsLock());
    instance->pendingNodeId = nodeId;
    instance->pendingRssi = rssi;
    instance->pendingMs = nowMs;
    instance->pendingValid = true;
}

void BoardFinderEngine::applyConfig(const Config &newCfg, bool restartIfArmed)
{
    cfg = newCfg;
    // Rider bursts: allowed for NORMAL (compat) and RIDER; never for BOARD.
    const bool allowBurst = cfg.ble_burst_enabled && cfg.role != boardfinder::DeviceRole::BOARD;
    t1000eBurstSetEnabled(allowBurst);
    t1000eBurstReapply();

    if (cfg.role != boardfinder::DeviceRole::BOARD) {
        disarm();
        return;
    }

    if (cfg.board_armed && cfg.rider_node_id != 0) {
        if (restartIfArmed || state == boardfinder::BoardState::DISARMED) {
            arm();
        }
    } else if (!cfg.board_armed) {
        disarm();
    }
}

boardfinder::ErrorCode BoardFinderEngine::arm()
{
    if (cfg.role != boardfinder::DeviceRole::BOARD)
        return boardfinder::ErrorCode::NOT_BOARD;
    if (cfg.rider_node_id == 0)
        return boardfinder::ErrorCode::RIDER_UNASSIGNED;

    // Idempotent: healthy monitoring/separated/sync sessions keep running.
    if (cfg.board_armed && state != boardfinder::BoardState::DISARMED && state != boardfinder::BoardState::ERROR) {
        return boardfinder::ErrorCode::NONE;
    }

    cfg.board_armed = true;
    synchronized = false;
    consecutiveMissed = 0;
    firstBurstMs = 0;
    phaseAnchorMs = 0;
    lastBurstStartMs = 0;
    inBurstObservation = false;
    windowOpen = false;
    windowSawRider = false;
    reacquireUntilMs = 0;
    periodsSinceReacquire = 0;
    stopGpsTracking();
    resetAcquisitionSoundFlags();
    boardfinder::stopBoardFinderSound();
    armStartedMs = millis();
    transitionTo(boardfinder::BoardState::DISCOVERING);
    startDiscoveryScan();
    enabled = true;
    setIntervalFromNow(50);
    LOG_INFO("[BF] ARM accepted -> DISCOVERING rider=0x%08x", cfg.rider_node_id);
    return boardfinder::ErrorCode::NONE;
}

boardfinder::ErrorCode BoardFinderEngine::disarm()
{
    const bool already = !cfg.board_armed && state == boardfinder::BoardState::DISARMED;
    cfg.board_armed = false;
    stopScan();
    stopGpsTracking();
    synchronized = false;
    consecutiveMissed = 0;
    windowOpen = false;
    inBurstObservation = false;
    resetAcquisitionSoundFlags();
    boardfinder::stopBoardFinderSound();
    transitionTo(boardfinder::BoardState::DISARMED);
    disable();
    if (!already)
        LOG_INFO("[BF] DISARM");
    return boardfinder::ErrorCode::NONE;
}

void BoardFinderEngine::resetAcquisitionSoundFlags()
{
    notFoundSoundPlayed = false;
}

BoardFinderEngine::StatusSnapshot BoardFinderEngine::status() const
{
    StatusSnapshot s{};
    s.device_role = cfg.role;
    s.rider_node_id = cfg.rider_node_id;
    s.board_armed = cfg.board_armed;
    s.board_state = state;
    s.synchronized = synchronized;
    s.consecutive_missed_bursts = consecutiveMissed;
    s.has_last_rider_rssi = hasRssi;
    s.last_rider_rssi = lastRssi;
    if (hasSeen) {
        s.has_last_rider_seen_age_seconds = true;
        s.last_rider_seen_age_seconds = (millis() - lastSeenMs) / 1000u;
    }
    s.gps_tracking_active = gpsTrackingActive;
    s.last_gps_report_status = lastGpsStatus;
    return s;
}

void BoardFinderEngine::transitionTo(boardfinder::BoardState next)
{
    if (state != next) {
        LOG_INFO("[BF] state %u -> %u", static_cast<unsigned>(state), static_cast<unsigned>(next));
        state = next;
    }
}

void BoardFinderEngine::startDiscoveryScan()
{
    windowOpen = true;
    windowSawRider = false;
    windowStartMs = millis();
    windowEndMs = 0;
    t1000eBoardScanStart();
}

void BoardFinderEngine::stopScan()
{
    t1000eBoardScanStop();
    windowOpen = false;
}

void BoardFinderEngine::openScanWindow(uint32_t nowMs)
{
    windowOpen = true;
    windowSawRider = false;
    windowStartMs = nowMs;
    windowEndMs = nowMs + kScanWindowMs;
    inBurstObservation = false;
    t1000eBoardScanStart();
    LOG_DEBUG("[BF] scan window open until %u", windowEndMs);
}

void BoardFinderEngine::closeScanWindow(uint32_t nowMs)
{
    if (!windowOpen)
        return;
    const bool saw = windowSawRider;
    stopScan();
    windowOpen = false;

    if (state == boardfinder::BoardState::MONITORING || state == boardfinder::BoardState::SEPARATED) {
        if (!saw) {
            onMissedWindow();
        } else {
            consecutiveMissed = 0;
        }
        // Schedule next nominal window: 3 s before next expected burst.
        const uint32_t nextBurst = expectedBurstStart(nowMs);
        nextWindowOpenMs = nextBurst - kScanPreMs;
        periodsSinceReacquire++;
        if (state == boardfinder::BoardState::SEPARATED && periodsSinceReacquire >= kReacquireEveryN) {
            periodsSinceReacquire = 0;
            reacquireUntilMs = nowMs + kReacquireScanMs;
            startDiscoveryScan();
            LOG_INFO("[BF] reacquisition scan for %u ms", kReacquireScanMs);
        }
    }
}

uint32_t BoardFinderEngine::expectedBurstStart(uint32_t nowMs) const
{
    if (phaseAnchorMs == 0)
        return nowMs + kBurstPeriodMs;
    uint32_t elapsed = nowMs - phaseAnchorMs;
    uint32_t periods = elapsed / kBurstPeriodMs;
    uint32_t candidate = phaseAnchorMs + (periods + 1) * kBurstPeriodMs;
    // If still inside current predicted burst window opening, use this period's start.
    uint32_t thisStart = phaseAnchorMs + periods * kBurstPeriodMs;
    if (nowMs + kScanPreMs < thisStart + kBurstDurationMs + kScanPostMs && nowMs < thisStart + kBurstPeriodMs / 2)
        return thisStart;
    return candidate;
}

void BoardFinderEngine::noteBurst(uint32_t burstStartMs, int8_t rssi)
{
    lastBurstStartMs = burstStartMs;
    lastRssi = rssi;
    hasRssi = true;
    lastSeenMs = burstStartMs;
    hasSeen = true;
    windowSawRider = true;
}

void BoardFinderEngine::onSyncComplete()
{
    synchronized = true;
    consecutiveMissed = 0;
    transitionTo(boardfinder::BoardState::MONITORING);
    boardfinder::playBoardFinderSound(boardfinder::BoardFinderSound::SYNCHRONIZED);
    // Next window around the following period.
    nextWindowOpenMs = phaseAnchorMs + kBurstPeriodMs - kScanPreMs;
    stopScan();
    windowOpen = false;
    LOG_INFO("[BF] synchronized");
}

void BoardFinderEngine::onMissedWindow()
{
    consecutiveMissed++;
    LOG_INFO("[BF] missed burst #%u", consecutiveMissed);
    if (state == boardfinder::BoardState::MONITORING && consecutiveMissed >= 2) {
        enterSeparated();
    }
}

void BoardFinderEngine::enterSeparated()
{
    transitionTo(boardfinder::BoardState::SEPARATED);
    boardfinder::playBoardFinderSound(boardfinder::BoardFinderSound::SEPARATION_DETECTED);
    gpsTrackingActive = true;
    lastGpsAttemptMs = 0;
    lastGpsStatus = boardfinder::GpsReportStatus::NONE;
    LOG_INFO("[BF] SEPARATED — GPS tracking on");
}

void BoardFinderEngine::recoverFromSeparated(uint32_t burstStartMs, int8_t rssi)
{
    // Recovery rule: one valid distinct Rider burst matching configured Node ID.
    noteBurst(burstStartMs, rssi);
    consecutiveMissed = 0;
    phaseAnchorMs = burstStartMs;
    stopGpsTracking();
    synchronized = true;
    reacquireUntilMs = 0;
    transitionTo(boardfinder::BoardState::MONITORING);
    nextWindowOpenMs = burstStartMs + kBurstPeriodMs - kScanPreMs;
    LOG_INFO("[BF] recovered to MONITORING");
}

void BoardFinderEngine::stopGpsTracking()
{
    gpsTrackingActive = false;
}

void BoardFinderEngine::maybeReportGps(uint32_t nowMs)
{
    if (!gpsTrackingActive)
        return;
    if (lastGpsAttemptMs != 0 && (nowMs - lastGpsAttemptMs) < kGpsReportIntervalMs)
        return;
    lastGpsAttemptMs = nowMs;

#if !MESHTASTIC_EXCLUDE_GPS
    if (gps && config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED) {
        gps->enable();
    }
#endif

    meshtastic_NodeInfoLite *node = nodeDB ? nodeDB->getMeshNode(nodeDB->getNodeNum()) : nullptr;
    if (!node || !nodeDB->hasValidPosition(node) || !nodeDB->hasLocalPositionSinceBoot()) {
        lastGpsStatus = boardfinder::GpsReportStatus::UNAVAILABLE;
        LOG_INFO("[BF] GPS unavailable — skip Position");
        return;
    }

    if (positionModule) {
        positionModule->sendOurPosition();
        lastGpsStatus = boardfinder::GpsReportStatus::OK;
        LOG_INFO("[BF] Position report sent");
    } else {
        lastGpsStatus = boardfinder::GpsReportStatus::FAILED;
    }
}

void BoardFinderEngine::handleObservationLocked(uint32_t nodeId, int8_t rssi, uint32_t nowMs)
{
    if (cfg.role != boardfinder::DeviceRole::BOARD || !cfg.board_armed)
        return;
    if (nodeId != cfg.rider_node_id)
        return;
    if (state == boardfinder::BoardState::DISARMED || state == boardfinder::BoardState::ERROR)
        return;

    if (inBurstObservation && boardfinder::sameBurst(currentBurstStartMs, nowMs, kBurstGroupMs)) {
        // Same burst — refresh RSSI only.
        lastRssi = rssi;
        hasRssi = true;
        lastSeenMs = nowMs;
        hasSeen = true;
        windowSawRider = true;
        return;
    }

    // New distinct burst.
    inBurstObservation = true;
    currentBurstStartMs = nowMs;
    noteBurst(nowMs, rssi);

    if (state == boardfinder::BoardState::DISCOVERING) {
        firstBurstMs = nowMs;
        phaseAnchorMs = nowMs;
        notFoundSoundPlayed = true; // suppress Signal C after first detection
        transitionTo(boardfinder::BoardState::SYNCHRONIZING);
        boardfinder::playBoardFinderSound(boardfinder::BoardFinderSound::RIDER_DETECTED);
        // Keep scanning through SYNCHRONIZING. Stopping here and relying on a later 9 s
        // window was missing the second Rider burst in the field (repeated Signal A only).
        windowOpen = true;
        windowEndMs = 0;
        nextWindowOpenMs = nowMs + kBurstPeriodMs - kScanPreMs;
        if (!t1000eBoardScanIsRunning())
            t1000eBoardScanStart();
        LOG_INFO("[BF] first burst @%u -> SYNCHRONIZING (scan continues)", nowMs);
        return;
    }

    if (state == boardfinder::BoardState::SYNCHRONIZING) {
        const uint32_t delta = nowMs - firstBurstMs;
        if (delta >= kPeriodMinMs && delta <= kPeriodMaxMs) {
            phaseAnchorMs = nowMs;
            LOG_INFO("[BF] second burst delta=%u ms -> sync", delta);
            onSyncComplete();
        } else if (delta > kPeriodMaxMs) {
            // Late but distinct — treat as new first burst and keep synchronizing.
            firstBurstMs = nowMs;
            phaseAnchorMs = nowMs;
            nextWindowOpenMs = nowMs + kBurstPeriodMs - kScanPreMs;
            LOG_WARN("[BF] sync period outlier delta=%u, re-anchor (no detect sound)", delta);
        } else {
            LOG_DEBUG("[BF] sync burst ignored early delta=%u", delta);
        }
        return;
    }

    if (state == boardfinder::BoardState::SEPARATED) {
        recoverFromSeparated(nowMs, rssi);
        stopScan();
        windowOpen = false;
        return;
    }

    if (state == boardfinder::BoardState::MONITORING) {
        // Update phase from observed burst start to limit drift.
        phaseAnchorMs = nowMs;
        consecutiveMissed = 0;
    }
}

int32_t BoardFinderEngine::runOnce()
{
    const uint32_t now = millis();
    boardfinder::boardFinderSoundPoll(now);

    // Drain scan observations.
    uint32_t nodeId = 0;
    int8_t rssi = 0;
    uint32_t obsMs = 0;
    bool have = false;
    {
        concurrency::LockGuard guard(&obsLock());
        if (pendingValid) {
            nodeId = pendingNodeId;
            rssi = pendingRssi;
            obsMs = pendingMs;
            pendingValid = false;
            have = true;
        }
    }
    if (have)
        handleObservationLocked(nodeId, rssi, obsMs);

    if (!cfg.board_armed || cfg.role != boardfinder::DeviceRole::BOARD) {
        return 500;
    }

    if (state == boardfinder::BoardState::DISCOVERING && firstBurstMs == 0 && !notFoundSoundPlayed &&
        (now - armStartedMs) >= kDiscoveryNotFoundMs) {
        notFoundSoundPlayed = true;
        boardfinder::playBoardFinderSound(boardfinder::BoardFinderSound::RIDER_NOT_FOUND);
        LOG_INFO("[BF] Rider not found in first 60s — continue discovery");
    }

    if (state == boardfinder::BoardState::DISCOVERING || state == boardfinder::BoardState::SYNCHRONIZING) {
        // Acquisition must keep the SoftDevice observer alive until MONITORING.
        if (!t1000eBoardScanIsRunning()) {
            LOG_WARN("[BF] acquisition scanner idle — restarting");
            t1000eBoardScanStart();
            windowOpen = true;
            windowEndMs = 0;
        }
        if ((now - armStartedMs) > kSyncTimeoutMs) {
            LOG_WARN("[BF] sync timeout");
            transitionTo(boardfinder::BoardState::ERROR);
            // Bounded retry: return to DISCOVERING (new acquisition attempt).
            armStartedMs = now;
            firstBurstMs = 0;
            inBurstObservation = false;
            resetAcquisitionSoundFlags();
            transitionTo(boardfinder::BoardState::DISCOVERING);
            startDiscoveryScan();
        }
    }

    if (state == boardfinder::BoardState::MONITORING || state == boardfinder::BoardState::SEPARATED) {
        if (reacquireUntilMs != 0) {
            if ((int32_t)(now - reacquireUntilMs) >= 0) {
                reacquireUntilMs = 0;
                if (!windowSawRider) {
                    stopScan();
                    windowOpen = false;
                    nextWindowOpenMs = expectedBurstStart(now) - kScanPreMs;
                }
            }
        } else if (!windowOpen && (int32_t)(now - nextWindowOpenMs) >= 0) {
            openScanWindow(now);
        }

        if (windowOpen && windowEndMs != 0 && (int32_t)(now - windowEndMs) >= 0) {
            closeScanWindow(now);
        }
    }

    if (state == boardfinder::BoardState::SEPARATED)
        maybeReportGps(now);

    return 50;
}

#endif
