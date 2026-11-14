# Board Finder Protocol Freeze

## Policy

1. **`protocol_version = 1` is the only authorized Board Finder protocol version.**
2. Firmware and Android must always send and accept version **1** only.
3. New firmware releases do **not** imply protocol version changes.
4. New Android releases do **not** imply protocol version changes.
5. Additive fields and commands do **not** trigger version increments.
6. Existing field numbers and command IDs are **immutable**.
7. Existing wire types and semantics are **immutable**.
8. Breaking changes require **explicit user approval** before implementation.
9. Agents (Cursor/Codex/etc.) must **not** create v2/v3/v4 without approval.
10. Any detected wire `protocol_version` other than `1` must fail automated validation.

## Canonical contract

See [`protocol/boardfinder.proto`](../protocol/boardfinder.proto).

Keep that file identical in the firmware and Android repositories.

## Compatibility rules

- Unknown optional fields: skip / ignore safely.
- Unknown commands: `ERROR_RESPONSE` with `UNSUPPORTED_COMMAND`.
- Wrong protocol version: `ERROR_RESPONSE` with `UNSUPPORTED_VERSION`.
- Partial `SET_CONFIG`: only update fields that are present; never invent resets.
- Transport remains Meshtastic Phone API + `PRIVATE_APP` (256). No custom GATT.

## Feature flags

Do **not** use `protocol_version` as a feature flag. Features are discovered by
commands and fields, not by bumping the version number.
