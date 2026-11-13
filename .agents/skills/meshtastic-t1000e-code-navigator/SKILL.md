---
name: meshtastic-t1000e-code-navigator
description: Analyzes the Meshtastic codebase for SenseCAP T1000-E: finds board/variant config, button/input handling, GNSS data flow, and telemetry/position messaging, then proposes insertion points for new T1000-E behavior. Use when the user says "ANALYZE_T1000E" or asks to navigate or analyze T1000-E code paths and hook points.
---

# Meshtastic T1000-E Code Navigator

When the user says **ANALYZE_T1000E** (or equivalent request), perform the following analysis. **Do not change any code.** Only find, list, describe, and propose insertion points.

---

## 1. Find and list

### 1.1 Board/variant configuration for T1000-E

Search and report:

- **Board/variant files**
  - `variants/nrf52840/tracker-t1000-e/variant.h` — pin mapping, `BUTTON_PIN`, GPS/LR1110/ADC/buzzer/T1000X sensor defines, `HAS_SCREEN 0`
  - `variants/nrf52840/tracker-t1000-e/platformio.ini` — env name `tracker-t1000-e`, `-DTRACKER_T1000_E`, build_flags, lib_deps
  - `boards/tracker-t1000-e.json` — board metadata, USB product, variant name

- **Key defines in variant.h**
  - `BUTTON_PIN (0+6)`, `BUTTON_ACTIVE_LOW` / `BUTTON_SENSE_TYPE`
  - `HAS_GPS`, `GNSS_AIROHA`, `GPS_RX_PIN`/`GPS_TX_PIN`, `PIN_GPS_EN`, `GPS_*` pins
  - `USE_LR1110`, LR1110/LR11X0 pin macros
  - `T1000X_SENSOR_EN`, `T1000X_VCC_PIN`/`NTC_PIN`/`LUX_PIN`
  - `HAS_SCREEN 0`

- **Platform wiring**
  - `src/platform/nrf52/architecture.h` — `#elif defined(TRACKER_T1000_E)` → `HW_VENDOR meshtastic_HardwareModel_TRACKER_T1000_E`

For each: **file path**, **relevant macros/structs**, **one-line role**.

---

### 1.2 Button/input events for T1000-E or similar small trackers

Search and report:

- **Variant**: `BUTTON_PIN` in `variants/nrf52840/tracker-t1000-e/variant.h` (P0.06).

- **Button/input flow**
  - `src/input/ButtonThread.h`, `src/input/ButtonThread.cpp` — `ButtonThread`, `initButton(ButtonConfig)`, OneButton, attachClick/LongPress/DoubleClick; emits events and registers with InputBroker.
  - `src/input/InputBroker.h`, `src/input/InputBroker.cpp` — `InputBroker`, `registerSource()`, `handleInputEvent()`, `input_broker_event` (e.g. `INPUT_BROKER_USER_PRESS`, `INPUT_BROKER_SEND_PING`, `INPUT_BROKER_GPS_TOGGLE`, `INPUT_BROKER_SHUTDOWN`).

- **Where T1000-E gets its button**
  - `src/main.cpp` (around 1119–1173): `#if defined(BUTTON_PIN)`; `_pinNum = BUTTON_PIN` (or `config.device.button_gpio`); with **no screen** (`!screen`), uses `userConfigNoScreen`: single = `USER_PRESS`, long = `NONE`, longLong = `SHUTDOWN`, double = `SEND_PING`, triple = `GPS_TOGGLE`. `UserButtonThread->initButton(userConfigNoScreen)`.

- **Who consumes input**
  - Any observer of `InputBroker` (e.g. via `inputBroker->addObserver(...)` or equivalent). Grep for `addObserver`/`observe` with `inputBroker` or `InputEvent` to list handlers.

For each: **file path**, **class/function/event**, **short description and how it connects** (variant → main → ButtonThread → InputBroker → observers).

---

### 1.3 GNSS/GPS data — where it’s retrieved and stored (current fix)

Search and report:

- **GPS driver and “current fix”**
  - `src/gps/GPS.h`, `src/gps/GPS.cpp` — class `GPS` (extends `OSThread`), member `meshtastic_Position p`, `GPSPowerState`; produces position fix and notifies observers.
  - `src/gps/GPSUpdateScheduling.h`, `src/gps/GPSUpdateScheduling.cpp` — when/how often GPS is woken and updated (scheduling).

- **Where the fix is stored**
  - `src/mesh/NodeDB.h`, `src/mesh/NodeDB.cpp` — local node position stored in node DB; “current fix” is the local node’s position (and possibly `nodeDB->hasLocalPositionSinceBoot()` etc.).
  - `PositionModule` subscribes to status/position and calls `handleNewPosition()` (see below).

- **T1000-E–specific GPS/sleep**
  - `src/sleep.cpp` — `#ifdef TRACKER_T1000_E` (and similar) for wake/sleep and GPS-related behavior.

For each: **file path**, **class/struct/function**, **what it does and how it connects** (GPS thread → position → NodeDB / PositionModule).

---

### 1.4 Text or telemetry messages built and sent to the Meshtastic channel

Search and report:

- **Position broadcasts**
  - `src/modules/PositionModule.h`, `src/modules/PositionModule.cpp` — `PositionModule::sendOurPosition()`, `sendOurPosition(NodeNum, bool, uint8_t)`, `allocPositionPacket()`, `runOnce()` (periodic/smart broadcast). Builds `meshtastic_Position` and sends to the mesh.
  - `src/mesh/MeshService.h`, `src/mesh/MeshService.cpp` — `trySendPosition(dest, wantReplies)` → calls `positionModule->sendOurPosition(...)`. “On position change” logic can trigger sends.

- **Other telemetry**
  - `src/modules/Telemetry/EnvironmentTelemetry.cpp` — `sendTelemetry()`; for T1000-E, `T1000xSensor` is used when `T1000X_SENSOR_EN` is defined (see `EnvironmentTelemetry.cpp` and `Sensor/T1000xSensor.cpp`).
  - `src/modules/Telemetry/DeviceTelemetry.cpp`, `PowerTelemetry.cpp`, `HealthTelemetry.cpp`, `AirQualityTelemetry.cpp` — `sendTelemetry()` for device/power/health/air metrics.

- **Trigger paths**
  - User/button: e.g. `INPUT_BROKER_SEND_PING` → code that calls `service->trySendPosition(NODENUM_BROADCAST, true)` (e.g. in `ExpressLRSFiveWay.cpp`, `MenuHandler.cpp`, `SystemCommandsModule.cpp`, `AdminModule.cpp`).

For each: **file path**, **function/module**, **what is built/sent and how it’s triggered** (config, timer, button, or “position changed”).

---

## 2. For each of the areas above, provide

- **File path** (relative to repo root).
- **Most relevant classes, structs, or functions.**
- **Short description** of what they do and how they connect to each other.

Summarize in a compact, scannable form (e.g. bullet or table) so the user can jump into the repo quickly.

---

## 3. Propose insertion points (no code changes)

### 3.1 Best places to hook new T1000-E–specific behavior

- **Special button semantics**
  - **Option A**: In `src/main.cpp`, under `#if defined(BUTTON_PIN)` and `#if defined(TRACKER_T1000_E)`, use a T1000-E–specific `ButtonConfig` (different single/double/triple/long assignments) before `UserButtonThread->initButton(...)`.
  - **Option B**: Add an observer of `InputBroker` that checks `#if defined(TRACKER_T1000_E)` and implements T1000-E–only actions (e.g. extra double-press behavior, LED/buzzer patterns). Hook registration in a T1000-E–guarded block in `main.cpp` or in a module that’s always loaded for T1000-E.

- **Board-specific features (e.g. “send telemetry on long-press”)**
  - Implement in a **shared module** (e.g. a small “TrackerActions” or existing module) with `#if defined(TRACKER_T1000_E)` blocks, so only T1000-E runs that logic. Trigger either from an InputBroker observer or from a new button event that you map in main.cpp for T1000-E only.

### 3.2 Where to add code: variant vs shared + conditionals

- **T1000-E variant only**
  - **Variant files**: New pins, new hardware capabilities, compile-time toggles. Put in `variants/nrf52840/tracker-t1000-e/variant.h` (or a T1000-E–only `.cpp` in that variant’s `build_src_filter`). Use for **hardware abstraction** (e.g. “this board has X pin”), not business logic.

- **Shared code + T1000-E conditionals**
  - **Use for behavior**: Button semantics, “send position on double-press”, special telemetry, LED/buzzer patterns. Prefer **shared modules or `main.cpp`** plus `#if defined(TRACKER_T1000_E)` (or `#ifdef TRACKER_T1000_E`) so other boards are unchanged and the intent is clear.
  - **Examples**: `src/sleep.cpp` already uses `#ifdef TRACKER_T1000_E`; new logic can follow that pattern in `src/main.cpp`, in an InputBroker observer, or in a module that runs on T1000-E.

- **Rule of thumb**
  - **Variant**: “What hardware does this board have?” (pins, peripherals, capabilities).
  - **Shared + `TRACKER_T1000_E`**: “What does this board do when the user does X?” (handlers, telemetry, UI-less flows).

---

## 4. Constraints

- **Do not** add, modify, or delete code. Only analyze and propose.
- Output: structured sections (1.1–1.4, 2, 3.1–3.2) with paths, symbols, and one-line descriptions.
- If a path or symbol has moved, say “assumed path X; if missing, search for Y” and suggest a grep pattern.

---

## Optional: quick search hints

- T1000-E define: `TRACKER_T1000_E`, `tracker-t1000-e`.
- Buttons: `BUTTON_PIN`, `ButtonThread`, `InputBroker`, `userConfigNoScreen`, `handleInputEvent`.
- GPS: `GPS.h`, `GPS::p`, `NodeDB`, `handleNewPosition`, `hasLocalPositionSinceBoot`.
- Position/telemetry send: `sendOurPosition`, `trySendPosition`, `PositionModule`, `sendTelemetry`, `T1000xSensor`.
