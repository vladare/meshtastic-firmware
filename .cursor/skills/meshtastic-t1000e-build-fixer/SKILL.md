---
name: meshtastic-t1000e-build-fixer
description: Fixes build errors after changes to Meshtastic firmware for T1000-E. Identifies the first real compile error, classifies it (missing include, wrong type, undefined symbol, macro misuse), and proposes minimal code patches. Use when the user pastes build logs from `pio run -e tracker-t1000-e` or asks to fix T1000-E compile errors.
---

# Meshtastic T1000-E Build Fixer

When the user pastes build logs from `pio run -e tracker-t1000-e` (or another T1000-E env), fix the **first** real compile error with a minimal, scope-limited patch.

---

## 1. Identify the first real error

- Ignore cascade errors that repeat the same symbol or “because it has no type” / “was not declared” follow-ups.
- Locate the **first** compiler error that introduces an undefined or invalid use.
- Extract and report:
  - **File path** (relative to repo root)
  - **Line number**
  - **Symbol** causing the error (function, variable, type, or macro)

---

## 2. Classify the error

Pick one and state it explicitly:

| Class | Meaning | Typical fix |
|-------|---------|-------------|
| **Missing include / forward declaration** | Symbol exists elsewhere but isn’t visible here | Add `#include` or `forward declaration` in the failing file |
| **Wrong type / wrong function signature** | Argument/return type or parameter count doesn’t match definition | Adjust call site or definition so types/signatures match |
| **Undefined symbol / wrong namespace** | Symbol not defined, or defined in another namespace/class | Define symbol, add include, or use correct qualifier (`::`, `ClassName::`) |
| **Misused macro / preprocessor condition** | Code compiled under wrong `#ifdef` or macro expands badly | Fix condition or macro definition so T1000-E path is correct |
| **Other** | None of the above | Describe clearly and then propose the minimal fix |

---

## 3. Propose a minimal fix

- **Target**: The exact file and location (function/block) to edit.
- **Patch**: Concrete code — full function or small diff — that removes the error.
- **Constraints**:
  - Prefer the smallest edit that makes the build succeed.
  - No large refactors; preserve current behavior unless the error is due to wrong behavior.
  - Add includes/forwards or fix types/signatures; avoid new libraries or heavy changes.

---

## 4. Respect scope

- Change **only** what’s needed to fix the compile error.
- Do not modify unrelated modules or other boards.
- Keep **SOS feature** behavior unchanged unless the error is directly in SOS code.

---

## 5. After proposing fixes

Remind the user to rerun:

```bash
pio run -e tracker-t1000-e
```

(If they use another T1000-E environment name, use that instead.)

---

## Examples

**User paste:**  
`.../NodeDB.cpp:123:45: error: 'SomeType' was not declared in this scope`

**Response shape:**

1. **First error:** `NodeDB.cpp:123` — `SomeType` not declared.
2. **Class:** Missing include / forward declaration.
3. **Fix:** Add `#include "path/to/SomeType.h"` at the top of `NodeDB.cpp` (or a forward declaration if only a pointer/reference is used).
4. **Scope:** No other files or boards touched.
5. **Next step:** Run `pio run -e tracker-t1000-e` to confirm.

---

**User paste:**  
`.../PositionModule.cpp:456: error: no matching function for call to 'foo(int)'`

**Response shape:**

1. **First error:** `PositionModule.cpp:456` — call to `foo(int)` has no matching function.
2. **Class:** Wrong type / wrong function signature (e.g. `foo` expects `uint32_t` or two args).
3. **Fix:** At line 456, change the call to match the declared signature (e.g. cast, or add the missing argument), or fix the declaration if it’s wrong.
4. **Scope:** Only the call site (or the single definition) changed.
5. **Next step:** Run `pio run -e tracker-t1000-e` to confirm.
