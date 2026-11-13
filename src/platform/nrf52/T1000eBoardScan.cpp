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
    if (gRunning)
        return true;

    Bluefruit.Scanner.setRxCallback(onScanReport);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.useActiveScan(false);
    // 100 ms interval, 50 ms window — enough for 100 ms Rider adv events without saturating radio.
    Bluefruit.Scanner.setIntervalMS(100, 50);
    Bluefruit.Scanner.clearFilters();
    if (!Bluefruit.Scanner.start(0)) {
        LOG_ERROR("[BF-SCAN] start failed");
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
    Bluefruit.Scanner.stop();
    gRunning = false;
    LOG_INFO("[BF-SCAN] stopped");
}

bool t1000eBoardScanIsRunning()
{
    return gRunning;
}

#endif
