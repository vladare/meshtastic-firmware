#if defined(TRACKER_T1000_E)

#include "T1000eBoardScan.h"
#include "configuration.h"
#include "modules/BoardFinder/boardfinder_adv.h"
#include <bluefruit.h>

namespace
{
T1000eBoardScanCallback gCallback = nullptr;
bool gRunning = false;

void onScanReport(ble_gap_evt_adv_report_t *report)
{
    // SoftDevice pauses scanning after every ADV report. Bluefruit does NOT resume when
    // the rx callback is invoked — the application must call Scanner.resume() (see Adafruit
    // central_scan examples). Without this, Board hears at most one advertisement after start.
    if (report != nullptr && gCallback != nullptr) {
        boardfinder::RiderAdvMatch match{};
        if (boardfinder::parseRiderServiceData(report->data.p_data, report->data.len, match) && match.matched) {
            gCallback(match.node_id, report->rssi, millis());
        }
    }

    if (gRunning)
        Bluefruit.Scanner.resume();
}
} // namespace

void t1000eBoardScanSetCallback(T1000eBoardScanCallback cb)
{
    gCallback = cb;
}

bool t1000eBoardScanStart()
{
    if (gRunning)
        return true;

    Bluefruit.Scanner.setRxCallback(onScanReport);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.useActiveScan(false);
    // Continuous scan (window == interval). Rider advertises every 100 ms during a 3 s burst.
    Bluefruit.Scanner.setIntervalMS(100, 100);
    Bluefruit.Scanner.clearFilters();
    if (!Bluefruit.Scanner.start(0)) {
        LOG_ERROR("[BF-SCAN] start failed");
        gRunning = false;
        return false;
    }
    gRunning = true;
    LOG_INFO("[BF-SCAN] started");
    return true;
}

void t1000eBoardScanStop()
{
    if (!gRunning)
        return;
    gRunning = false;
    Bluefruit.Scanner.stop();
    LOG_INFO("[BF-SCAN] stopped");
}

bool t1000eBoardScanIsRunning()
{
    return gRunning;
}

#endif
