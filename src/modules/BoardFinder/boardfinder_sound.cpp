#include "boardfinder_sound.h"

#if defined(TRACKER_T1000_E)
#include "NodeDB.h"
#include "configuration.h"
#include <Arduino.h>
#endif

namespace boardfinder
{
namespace
{
constexpr size_t kMaxSteps = 12;

void appendTone(ToneStep *out, size_t &n, size_t outMax, uint16_t freq, uint16_t dur)
{
    if (n >= outMax)
        return;
    out[n++] = ToneStep{freq, dur};
}

void appendGap(ToneStep *out, size_t &n, size_t outMax, uint16_t dur)
{
    appendTone(out, n, outMax, 0, dur);
}

#if defined(TRACKER_T1000_E)
SoundScheduler gScheduler;
int gBuzzerPin = -1;

int resolveBuzzerPin()
{
    if (config.device.buzzer_gpio)
        return static_cast<int>(config.device.buzzer_gpio);
#if defined(PIN_BUZZER)
    return PIN_BUZZER;
#else
    return -1;
#endif
}

void applyAction(const SoundAction &action)
{
    gBuzzerPin = resolveBuzzerPin();
    if (gBuzzerPin < 0)
        return;

#if defined(BUZZER_EN_PIN)
    pinMode(BUZZER_EN_PIN, OUTPUT);
    digitalWrite(BUZZER_EN_PIN, HIGH);
#endif

    if (action.type == SoundAction::Type::StartTone && action.freq_hz > 0) {
        tone(gBuzzerPin, action.freq_hz, action.duration_ms);
    } else {
        noTone(gBuzzerPin);
    }
}

bool buzzerAllowed()
{
    return config.device.buzzer_mode != meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED;
}
#endif
} // namespace

uint8_t soundPriority(BoardFinderSound sound)
{
    switch (sound) {
    case BoardFinderSound::SEPARATION_DETECTED:
        return 4;
    case BoardFinderSound::SYNCHRONIZED:
        return 3;
    case BoardFinderSound::RIDER_DETECTED:
        return 2;
    case BoardFinderSound::RIDER_NOT_FOUND:
        return 1;
    default:
        return 0;
    }
}

size_t soundPattern(BoardFinderSound sound, ToneStep *out, size_t outMax)
{
    if (out == nullptr || outMax == 0)
        return 0;
    size_t n = 0;
    switch (sound) {
    case BoardFinderSound::RIDER_DETECTED:
        // HIGH HIGH HIGH
        appendTone(out, n, outMax, HIGH_TONE_HZ, TONE_DURATION_MS);
        appendGap(out, n, outMax, TONE_GAP_MS);
        appendTone(out, n, outMax, HIGH_TONE_HZ, TONE_DURATION_MS);
        appendGap(out, n, outMax, TONE_GAP_MS);
        appendTone(out, n, outMax, HIGH_TONE_HZ, TONE_DURATION_MS);
        break;
    case BoardFinderSound::SYNCHRONIZED:
        // HIGH HIGH HIGH — GROUP_GAP — LOW LOW LOW
        appendTone(out, n, outMax, HIGH_TONE_HZ, TONE_DURATION_MS);
        appendGap(out, n, outMax, TONE_GAP_MS);
        appendTone(out, n, outMax, HIGH_TONE_HZ, TONE_DURATION_MS);
        appendGap(out, n, outMax, TONE_GAP_MS);
        appendTone(out, n, outMax, HIGH_TONE_HZ, TONE_DURATION_MS);
        appendGap(out, n, outMax, GROUP_GAP_MS);
        appendTone(out, n, outMax, LOW_TONE_HZ, TONE_DURATION_MS);
        appendGap(out, n, outMax, TONE_GAP_MS);
        appendTone(out, n, outMax, LOW_TONE_HZ, TONE_DURATION_MS);
        appendGap(out, n, outMax, TONE_GAP_MS);
        appendTone(out, n, outMax, LOW_TONE_HZ, TONE_DURATION_MS);
        break;
    case BoardFinderSound::RIDER_NOT_FOUND:
        // LOW LOW LOW
        appendTone(out, n, outMax, LOW_TONE_HZ, TONE_DURATION_MS);
        appendGap(out, n, outMax, TONE_GAP_MS);
        appendTone(out, n, outMax, LOW_TONE_HZ, TONE_DURATION_MS);
        appendGap(out, n, outMax, TONE_GAP_MS);
        appendTone(out, n, outMax, LOW_TONE_HZ, TONE_DURATION_MS);
        break;
    case BoardFinderSound::SEPARATION_DETECTED:
        // LOW — 400 ms — LOW
        appendTone(out, n, outMax, LOW_TONE_HZ, TONE_DURATION_MS);
        appendGap(out, n, outMax, SEPARATION_GAP_MS);
        appendTone(out, n, outMax, LOW_TONE_HZ, TONE_DURATION_MS);
        break;
    default:
        break;
    }
    return n;
}

void SoundScheduler::load(BoardFinderSound sound)
{
    stepCount_ = soundPattern(sound, steps_, kMaxSteps);
    stepIndex_ = 0;
    active_ = stepCount_ > 0 ? sound : BoardFinderSound::NONE;
    waitingFirst_ = stepCount_ > 0;
    stepEndsAtMs_ = 0;
}

bool SoundScheduler::request(BoardFinderSound sound)
{
    if (sound == BoardFinderSound::NONE)
        return false;
    if (isPlaying() && soundPriority(sound) <= soundPriority(active_))
        return false;
    load(sound);
    return active_ != BoardFinderSound::NONE;
}

void SoundScheduler::stop()
{
    active_ = BoardFinderSound::NONE;
    stepCount_ = 0;
    stepIndex_ = 0;
    waitingFirst_ = false;
    stepEndsAtMs_ = 0;
}

bool SoundScheduler::tick(uint32_t nowMs, SoundAction &action)
{
    action = SoundAction{};
    if (active_ == BoardFinderSound::NONE || stepCount_ == 0)
        return false;

    if (waitingFirst_) {
        waitingFirst_ = false;
        stepIndex_ = 0;
        const ToneStep &s = steps_[0];
        stepEndsAtMs_ = nowMs + s.duration_ms;
        if (s.freq_hz == 0) {
            action.type = SoundAction::Type::Silence;
            action.duration_ms = s.duration_ms;
        } else {
            action.type = SoundAction::Type::StartTone;
            action.freq_hz = s.freq_hz;
            action.duration_ms = s.duration_ms;
        }
        return true;
    }

    if ((int32_t)(nowMs - stepEndsAtMs_) < 0)
        return false;

    stepIndex_++;
    if (stepIndex_ >= stepCount_) {
        active_ = BoardFinderSound::NONE;
        stepCount_ = 0;
        action.type = SoundAction::Type::Silence;
        return true;
    }

    const ToneStep &s = steps_[stepIndex_];
    stepEndsAtMs_ = nowMs + s.duration_ms;
    if (s.freq_hz == 0) {
        action.type = SoundAction::Type::Silence;
        action.duration_ms = s.duration_ms;
    } else {
        action.type = SoundAction::Type::StartTone;
        action.freq_hz = s.freq_hz;
        action.duration_ms = s.duration_ms;
    }
    return true;
}

#if defined(TRACKER_T1000_E)
void playBoardFinderSound(BoardFinderSound sound)
{
    if (!buzzerAllowed())
        return;
    if (!gScheduler.request(sound))
        return;
    // Preempt any previous PWM tone immediately; pattern starts on next poll.
    gBuzzerPin = resolveBuzzerPin();
    if (gBuzzerPin >= 0)
        noTone(gBuzzerPin);
}

void stopBoardFinderSound()
{
    gScheduler.stop();
    gBuzzerPin = resolveBuzzerPin();
    if (gBuzzerPin >= 0)
        noTone(gBuzzerPin);
}

void boardFinderSoundPoll(uint32_t nowMs)
{
    SoundAction action{};
    if (gScheduler.tick(nowMs, action))
        applyAction(action);
}
#endif

} // namespace boardfinder
