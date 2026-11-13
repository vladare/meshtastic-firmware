#include "boardfinder_economy.h"

namespace boardfinder
{
namespace
{

ReportingPhase computePhase(bool active, const EconomySettings &settings, uint32_t separatedAtMs, uint32_t nowMs)
{
    if (!active)
        return ReportingPhase::INACTIVE;
    if (!settings.economy_enabled)
        return ReportingPhase::NORMAL;
    const uint32_t elapsedS = (nowMs - separatedAtMs) / 1000u;
    return (elapsedS >= settings.economy_after_seconds) ? ReportingPhase::ECONOMY : ReportingPhase::NORMAL;
}

} // namespace

EconomySettings defaultEconomySettings()
{
    return EconomySettings{};
}

bool validateEconomySettings(const EconomySettings &s)
{
    if (s.economy_after_seconds < MIN_ECONOMY_AFTER_S || s.economy_after_seconds > MAX_ECONOMY_AFTER_S)
        return false;
    if (s.normal_gps_interval_seconds < MIN_NORMAL_GPS_S || s.normal_gps_interval_seconds > MAX_NORMAL_GPS_S)
        return false;
    if (s.economy_gps_interval_seconds < MIN_ECONOMY_GPS_S || s.economy_gps_interval_seconds > MAX_ECONOMY_GPS_S)
        return false;
    if (s.economy_gps_interval_seconds < s.normal_gps_interval_seconds)
        return false;
    return true;
}

void GpsReportScheduler::reset()
{
    active_ = false;
    phase_ = ReportingPhase::INACTIVE;
    separatedAtMs_ = 0;
    lastAttemptMs_ = 0;
    hasAttempt_ = false;
}

void GpsReportScheduler::onEnterSeparated(uint32_t nowMs)
{
    active_ = true;
    separatedAtMs_ = nowMs;
    lastAttemptMs_ = 0;
    hasAttempt_ = false;
    refreshPhase(nowMs);
}

void GpsReportScheduler::setSettings(const EconomySettings &s, uint32_t nowMs)
{
    settings_ = s;
    if (active_)
        refreshPhase(nowMs);
}

void GpsReportScheduler::refreshPhase(uint32_t nowMs)
{
    phase_ = computePhase(active_, settings_, separatedAtMs_, nowMs);
}

uint32_t GpsReportScheduler::intervalSeconds() const
{
    if (!active_)
        return 0;
    if (phase_ == ReportingPhase::ECONOMY)
        return settings_.economy_gps_interval_seconds;
    return settings_.normal_gps_interval_seconds;
}

uint32_t GpsReportScheduler::separationElapsedSeconds(uint32_t nowMs) const
{
    if (!active_)
        return 0;
    return (nowMs - separatedAtMs_) / 1000u;
}

bool GpsReportScheduler::nextDueSeconds(uint32_t nowMs, uint32_t &dueSeconds) const
{
    dueSeconds = 0;
    if (!active_)
        return false;

    const ReportingPhase ph = computePhase(active_, settings_, separatedAtMs_, nowMs);
    const uint32_t intervalS =
        (ph == ReportingPhase::ECONOMY) ? settings_.economy_gps_interval_seconds : settings_.normal_gps_interval_seconds;
    const uint32_t intervalMs = intervalS * 1000u;

    if (!hasAttempt_) {
        dueSeconds = 0;
        return true;
    }
    const uint32_t elapsed = nowMs - lastAttemptMs_;
    if (elapsed >= intervalMs) {
        dueSeconds = 0;
        return true;
    }
    dueSeconds = (intervalMs - elapsed + 999u) / 1000u;
    return true;
}

bool GpsReportScheduler::isDue(uint32_t nowMs) const
{
    uint32_t due = 0;
    if (!nextDueSeconds(nowMs, due))
        return false;
    return due == 0;
}

void GpsReportScheduler::markAttempted(uint32_t nowMs)
{
    hasAttempt_ = true;
    lastAttemptMs_ = nowMs;
}

} // namespace boardfinder
