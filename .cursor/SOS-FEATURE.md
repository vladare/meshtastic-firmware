# SOS Feature (T1000-E / screenless trackers)

Brief reference for the SOS flow and UX. Implementation lives in `SystemCommandsModule`, `TextMessageModule`, and `ExternalNotificationModule`.

## Opt-in transmission (Board Finder prefs)

- Persistent flag: `sos_transmission_enabled` (default **false**).
- Stored in Board Finder flash prefs (`PrefsRecordV4`); exposed as PRIVATE_APP field **24** (`protocol_version` remains **1**).
- When disabled, long-press SOS trigger is ignored (no ALERT/TEXT/Position, no sender ACK audio).
- Reception, SOS-ACK, and alarm behavior are unchanged.

## Channel policy (known upstream consideration)

SOS transmits on the device primary channel index (typically `node->channel` / 0). Firmware does **not** enforce private-channel-only SOS. Privacy depends on channel configuration. Do not silently change this policy without a dedicated design review.

## Flow

1. **Sender** (only if `sos_transmission_enabled`): User long-presses (≥2 s, <5 s) → `SystemCommandsModule` sends ALERT_APP "SOS" + TEXT_MESSAGE_APP "SOS: ..." (with or without GPS). Calls `onSosSent()` to reset ACK state.
2. **Receiver**: Receives SOS → plays distinct SOS alarm; may send SOS-ACK DM (TEXT_MESSAGE_APP "SOS-ACK") back to sender. Repeats alarm on a schedule until locally acknowledged (button).
3. **Sender**: Receives SOS-ACK → `scheduleSosAckPlayback()`; ~2–3 s later one "ta-daa" confirmation sound. At most one sound per SOS gesture; timeout 10 s if no play.

## Expected UX

- **Sender**: Press and hold → release → (optional) ~2–3 s later "ta-daa" = someone received it. No immediate chirp.
- **Receiver**: SOS alarm (distinct from normal message); repeats every 5 min (first 15 min) then every 15 min until user presses button to acknowledge.

## Known limitation

With more than two devices, multiple receivers may each send SOS-ACK for the same SOS.
  The sender plays at most one confirmation sound per SOS gesture
  (dedup by `ackPlayedForCurrentSos`). No protocol change; behavior is defensive.
- SOS-ACK arriving after the sender timeout (~10 s) is ignored for audio playback.

