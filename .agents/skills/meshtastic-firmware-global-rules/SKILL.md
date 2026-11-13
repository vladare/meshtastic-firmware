---
name: meshtastic-firmware-global-rules
description: Applies Meshtastic firmware development rules for multi-board (focus SenseCAP T1000-E), PlatformIO builds, and minimal scope changes. Use when editing Meshtastic firmware, adding T1000-E features, or working in this repository.
---

# Meshtastic Firmware – Global Rules

You are working inside the official Meshtastic firmware repository.

## General

- **Target platform**: Meshtastic multi-board firmware. Primary board: SenseCAP Card Tracker T1000-E.
- **Build system**: PlatformIO.
- **T1000-E environment**: `tracker-t1000-e`. Do not change this name unless the user explicitly gives another one.
- Prefer **minimal, localized changes** instead of large refactors.
- Do **not** modify `platformio.ini`, bootloader code, or linker scripts unless explicitly requested.
- Keep changes **board-specific** when possible (e.g. `#ifdef`, `#if defined(TRACKER_T1000_E)`, or per-board config).

## Coding

- Follow the **existing coding style** in each file (naming, logging, indentation).
- Use **small, self-contained functions**.
- New config options must:
  - Be grouped with similar options.
  - Preserve current behavior as default (feature off until explicitly triggered).
- New behavior must **fail gracefully** if prerequisites are missing (e.g. no GPS fix, feature disabled).

## Build

- Before and after changes, ensure T1000-E firmware compiles:
  ```bash
  pio run -e tracker-t1000-e
  ```
- If compilation fails, **first** try to fix missing includes, forward declarations, or wrong types.
- Do not add arbitrary libraries or heavy dependencies.

## Scope

- Change **only** the code needed for the current task.
- Do not “clean up” or refactor unrelated modules unless explicitly requested.
