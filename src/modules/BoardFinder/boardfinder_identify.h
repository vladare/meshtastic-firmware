#pragma once

#include <cstdint>

namespace boardfinder
{

constexpr uint32_t IDENTIFY_DURATION_MS = 5000;
constexpr uint32_t IDENTIFY_HALF_PERIOD_MS = 125; // 4 Hz: 125 ms on / 125 ms off

/**
 * Pure non-blocking Identify LED timeline (unit-testable without Arduino).
 * Restarting while active replaces the window; LED ownership ends automatically.
 */
class IdentifyController
{
  public:
    void start(uint32_t nowMs);
    void stop();
    bool isActive() const { return active_; }
    bool ownsLed() const { return active_; }

    // Returns true when the physical LED should be rewritten; ledOn is the desired lit state.
    bool tick(uint32_t nowMs, bool &ledOn);

  private:
    bool active_ = false;
    bool ledOn_ = false;
    uint32_t endsAtMs_ = 0;
    uint32_t nextToggleAtMs_ = 0;
};

// Firmware facade (TRACKER_T1000_E).
void startBoardFinderIdentify();
bool boardFinderIdentifyOwnsLed();

} // namespace boardfinder
