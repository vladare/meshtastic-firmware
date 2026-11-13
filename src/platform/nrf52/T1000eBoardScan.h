#pragma once

#if defined(TRACKER_T1000_E)

#include <stdint.h>

// Board Finder BLE observer (SoftDevice scan via Bluefruit.Scanner).
// Coexists with connectable advertising / Phone API GATT. Does not create connections.

typedef void (*T1000eBoardScanCallback)(uint32_t nodeId, int8_t rssi, uint32_t nowMs);

void t1000eBoardScanSetCallback(T1000eBoardScanCallback cb);
bool t1000eBoardScanStart();
void t1000eBoardScanStop();
bool t1000eBoardScanIsRunning();

#endif
