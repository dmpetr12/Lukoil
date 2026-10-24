# Single-meter test relay design

## Intent

During a cabinet test, relay 1 removes the normal working voltage exactly as in the existing test mode. All lighting lines remain enabled through their working relay paths (keys 3, 5, 7, ...), while one shared meter is connected to at most one line through keys 4, 6, 8, ....

## Sequence

- Test start energizes the common test relay 1 and enables every line's working path.
- All measurement relays remain off during the common warm-up period.
- Near the end of the test, each enabled line is measured for 10 seconds.
- Before selecting a line, all measurement relays are switched off.
- A 300 ms break-before-make gap is observed, then only the selected line's measurement relay is switched on.
- The selected line's working relay remains in its normal enabled state; the old transfer step that disabled it is removed.
- Stop, fire, or cancellation cancels the switching timer and leaves all measurement relays off.

## Constraints

- Never energize two measurement relays at the same time.
- Preserve the existing normally-closed semantics of working relays.
- Preserve relay 1 behavior and the existing Modbus output path.
- Do not redesign unrelated test scheduling, UI, or measurement tolerance logic.

## Approach

Replace the three-state overlap/transfer sequence with two states: disconnected and connected. `TestController` first runs the cabinet in no-measurement test mode for warm-up, then starts the first measurement step. `LineIoManager` enforces the 300 ms all-off interval whenever a measurement line is selected.
