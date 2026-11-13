#pragma once

#include <cstddef>
#include <cstdint>

namespace boardfinder
{

// Central Board Finder tone constants (adjust here).
constexpr uint16_t HIGH_TONE_HZ = 2400;
constexpr uint16_t LOW_TONE_HZ = 1200;
constexpr uint16_t TONE_DURATION_MS = 120;
constexpr uint16_t TONE_GAP_MS = 100;
constexpr uint16_t GROUP_GAP_MS = 250;
constexpr uint16_t SEPARATION_GAP_MS = 400;

enum class BoardFinderSound : uint8_t {
    NONE = 0,
    RIDER_NOT_FOUND = 1,      // priority 1 — Signal C
    RIDER_DETECTED = 2,       // priority 2 — Signal A
    SYNCHRONIZED = 3,         // priority 3 — Signal B
    SEPARATION_DETECTED = 4,  // priority 4 — Signal D
};

struct ToneStep {
    uint16_t freq_hz; // 0 = silence
    uint16_t duration_ms;
};

struct SoundAction {
    enum class Type : uint8_t { Idle, StartTone, Silence } type = Type::Idle;
    uint16_t freq_hz = 0;
    uint16_t duration_ms = 0;
};

uint8_t soundPriority(BoardFinderSound sound);

// Fills out[] with the pattern steps; returns step count (0 if unknown).
size_t soundPattern(BoardFinderSound sound, ToneStep *out, size_t outMax);

/**
 * Pure non-blocking pattern scheduler (unit-testable without Arduino).
 * Higher-priority requests replace a lower-priority active pattern.
 * Equal/lower priority while busy is ignored (no queue).
 */
class SoundScheduler
{
  public:
    bool request(BoardFinderSound sound);
    void stop();
    bool isPlaying() const { return active_ != BoardFinderSound::NONE; }
    BoardFinderSound active() const { return active_; }

    // Advance timeline. Returns true when a new hardware action should be applied.
    bool tick(uint32_t nowMs, SoundAction &action);

  private:
    void load(BoardFinderSound sound);

    BoardFinderSound active_ = BoardFinderSound::NONE;
    ToneStep steps_[12]{};
    size_t stepCount_ = 0;
    size_t stepIndex_ = 0;
    uint32_t stepEndsAtMs_ = 0;
    bool waitingFirst_ = false;
};

// Firmware facade (TRACKER_T1000_E): drives PWM buzzer from the scheduler.
void playBoardFinderSound(BoardFinderSound sound);
void stopBoardFinderSound();
void boardFinderSoundPoll(uint32_t nowMs);

} // namespace boardfinder
