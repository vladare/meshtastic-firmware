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
    if (report == nullptr || gCallback == nullptr)
        return;

    boardfinder::RiderAdvMatch match{};
    if (!boardfinder::parseRiderServiceData(report->data.p_data, report->data.len, match) || !match.matched)
        return;

    gCallback(match.node_id, report->rssi, millis());
}
} // namespace

void t1000eBoardScanSetCallback(T1000eBoardScanCallback cb)
{
    gCallback = cb;
}

bool t1000eBoardScanStart()
{
    if (gRunning) {
        // SoftDevice can stop reporting after errors; ensure callback stays armed.
        if (!Bluefruit.Scanner.isRunning()) {
            LOG_WARN("[BF-SCAN] flag set but scanner idle — restarting");
            gRunning = false;
        } else {
            return true;
        }
    }

    Bluefruit.Scanner.setRxCallback(onScanReport);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.useActiveScan(false);
    // Window == interval → continuous scan. Rider advertises every 100 ms during a 3 s burst;
    // a 50% duty cycle previously risked missing the entire second sync window.
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
    if (!gRunning && !Bluefruit.Scanner.isRunning())
        return;
    Bluefruit.Scanner.stop();
    gRunning = false;
    LOG_INFO("[BF-SCAN] stopped");
}

bool t1000eBoardScanIsRunning()
{
    return gRunning;
}

#endif
