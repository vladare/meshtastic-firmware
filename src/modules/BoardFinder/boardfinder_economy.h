#pragma once

#include "boardfinder_protocol.h"
#include <cstdint>

namespace boardfinder
{

constexpr uint32_t DEFAULT_ECONOMY_AFTER_S = 3600;
constexpr uint32_t DEFAULT_NORMAL_GPS_S = 60;
constexpr uint32_t DEFAULT_ECONOMY_GPS_S = 300;
constexpr uint32_t MIN_ECONOMY_AFTER_S = 300;   // 5 minutes
constexpr uint32_t MAX_ECONOMY_AFTER_S = 14400; // 240 minutes
constexpr uint32_t MIN_NORMAL_GPS_S = 30;
constexpr uint32_t MAX_NORMAL_GPS_S = 600;
constexpr uint32_t MIN_ECONOMY_GPS_S = 60;
constexpr uint32_t MAX_ECONOMY_GPS_S = 3600;

struct EconomySettings {
    bool economy_enabled = true;
    uint32_t economy_after_seconds = DEFAULT_ECONOMY_AFTER_S;
    uint32_t normal_gps_interval_seconds = DEFAULT_NORMAL_GPS_S;
    uint32_t economy_gps_interval_seconds = DEFAULT_ECONOMY_GPS_S;
};

bool validateEconomySettings(const EconomySettings &s);

EconomySettings defaultEconomySettings();

/**
 * Pure GPS reporting phase scheduler for SEPARATED (unit-testable).
 * Does not touch flash or hardware; Engine owns GPS transmission.
 */
class GpsReportScheduler
{
  public:
    void reset();
    void onEnterSeparated(uint32_t nowMs);
    void setSettings(const EconomySettings &s, uint32_t nowMs);

    bool active() const { return active_; }
    ReportingPhase phase() const { return phase_; }
    EconomySettings settings() const { return settings_; }
    uint32_t intervalSeconds() const;
    uint32_t separationElapsedSeconds(uint32_t nowMs) const;
    // true when active; dueSeconds=0 means report is due now.
    bool nextDueSeconds(uint32_t nowMs, uint32_t &dueSeconds) const;
    bool isDue(uint32_t nowMs) const;
    void markAttempted(uint32_t nowMs);
    void refreshPhase(uint32_t nowMs);

  private:
    EconomySettings settings_ = defaultEconomySettings();
    bool active_ = false;
    ReportingPhase phase_ = ReportingPhase::INACTIVE;
    uint32_t separatedAtMs_ = 0;
    uint32_t lastAttemptMs_ = 0;
    bool hasAttempt_ = false;
};

} // namespace boardfinder
