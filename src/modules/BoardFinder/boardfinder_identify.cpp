#include "boardfinder_identify.h"
#include "boardfinder_sound.h"

#if defined(TRACKER_T1000_E)
#include "configuration.h"
#include "concurrency/OSThread.h"
#include <Arduino.h>
#endif

namespace boardfinder
{

void IdentifyController::start(uint32_t nowMs)
{
    active_ = true;
    ledOn_ = true;
    endsAtMs_ = nowMs + IDENTIFY_DURATION_MS;
    nextToggleAtMs_ = nowMs + IDENTIFY_HALF_PERIOD_MS;
}

void IdentifyController::stop()
{
    active_ = false;
    ledOn_ = false;
    endsAtMs_ = 0;
    nextToggleAtMs_ = 0;
}

bool IdentifyController::tick(uint32_t nowMs, bool &ledOn)
{
    if (!active_)
        return false;

    if ((int32_t)(nowMs - endsAtMs_) >= 0) {
        active_ = false;
        ledOn_ = false;
        ledOn = false;
        return true;
    }

    if ((int32_t)(nowMs - nextToggleAtMs_) >= 0) {
        ledOn_ = !ledOn_;
        nextToggleAtMs_ = nowMs + IDENTIFY_HALF_PERIOD_MS;
        ledOn = ledOn_;
        return true;
    }

    ledOn = ledOn_;
    return false;
}

#if defined(TRACKER_T1000_E)
namespace
{

class IdentifyThread : private concurrency::OSThread
{
  public:
    IdentifyThread() : concurrency::OSThread("BfIdentify") { disable(); }

    void begin()
    {
        const uint32_t now = millis();
        ctrl_.start(now);
        applyLed(true);
        playBoardFinderSound(BoardFinderSound::IDENTIFY);
        // Kick the first tone immediately; subsequent steps come from runOnce().
        boardFinderSoundPoll(now);
        // disable() clears enabled; setInterval alone does not re-arm the thread.
        enabled = true;
        setIntervalFromNow(0);
    }

    bool ownsLed() const { return ctrl_.ownsLed(); }

  protected:
    int32_t runOnce() override
    {
        const uint32_t now = millis();
        boardFinderSoundPoll(now);

        bool ledOn = false;
        if (ctrl_.tick(now, ledOn)) {
            if (ctrl_.isActive()) {
                applyLed(ledOn);
            } else {
                releaseLed();
                return disable();
            }
        }

        if (!ctrl_.isActive()) {
            releaseLed();
            return disable();
        }
        return static_cast<int32_t>(IDENTIFY_HALF_PERIOD_MS / 2);
    }

  private:
    void applyLed(bool on)
    {
#ifdef LED_POWER
        pinMode(LED_POWER, OUTPUT);
        digitalWrite(LED_POWER, on ? LED_STATE_ON : LED_STATE_OFF);
#else
        (void)on;
#endif
    }

    void releaseLed()
    {
        // Drop ownership first so StatusLEDModule can write on its next tick.
        ctrl_.stop();
#ifdef LED_POWER
        // Do not force ON — baseline T1000-E heartbeat is mostly OFF.
        digitalWrite(LED_POWER, LED_STATE_OFF);
#endif
    }

    IdentifyController ctrl_{};
};

IdentifyThread *gIdentifyThread = nullptr;

IdentifyThread &identifyThread()
{
    if (!gIdentifyThread)
        gIdentifyThread = new IdentifyThread();
    return *gIdentifyThread;
}

} // namespace

void startBoardFinderIdentify()
{
    identifyThread().begin();
}

bool boardFinderIdentifyOwnsLed()
{
    return gIdentifyThread && gIdentifyThread->ownsLed();
}
#endif

} // namespace boardfinder
