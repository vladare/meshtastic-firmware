#pragma once

#if defined(TRACKER_T1000_E)

#include <stdint.h>

// Experimental Board Finder BLE burst control for SenseCAP T1000-E.
// Implemented in NRF52Bluetooth.cpp. Default enabled state is false.

void t1000eBurstSetEnabled(bool enabled);
bool t1000eBurstIsEnabled();

// Cancel any active/pending burst and re-arm from the current enabled flag
// if a phone is connected (15 s delay before the first burst).
void t1000eBurstReapply();

#endif
