# Single-meter test relay design

## Intent

During a cabinet test, relay 1 removes the normal working voltage exactly as in the existing test mode. All lighting lines remain enabled through their working relay paths (keys 3, 5, 7, ...), while one shared meter is connected to at most one line through keys 4, 6, 8, ....

## Sequence

- Test start energizes the common test relay 1 and enables every line's working path.
- All measurement relays remain off during the common warm-up period.
- Near the end of the test, each enabled line is selected for a single fresh voltage/current sample after 1000 ms of meter settling.
- Before selecting a line, all measurement relays are switched off.
- A 300 ms break-before-make gap is observed, then only the selected line's measurement relay is switched on.
- The gap starts only after successful Modbus write acknowledgements from every relay module for the current switching sequence. Old acknowledgements cannot advance a new sequence.
- After its ON write is acknowledged, the controller waits 1000 ms, then submits a dedicated high-priority DJSF1352 read with a unique sequence number. Only the matching successful response is used for this line. Periodic or old responses cannot finish a line. Failure to connect within 10 seconds or receive the requested sample within 5 seconds aborts the test.
- The selected line's working relay remains in its normal enabled state; the old transfer step that disabled it is removed.
- Stop, fire, or cancellation cancels the switching timer and leaves all measurement relays off.

## Constraints

- Never energize two measurement relays at the same time.
- Preserve the existing normally-closed semantics of working relays.
- Preserve relay 1 behavior and the existing Modbus output path.
- Do not redesign unrelated test scheduling, UI, or measurement tolerance logic.

## Approach

Replace the three-state overlap/transfer sequence with two states: disconnected and connected. `TestController` first runs the cabinet in no-measurement test mode for warm-up, then starts the first measurement step. `LineIoManager` enforces the 300 ms all-off interval whenever a measurement line is selected.

Transport acknowledgement confirms the module accepted the command; it does not detect welded physical contacts. Real switching times and wiring require validation on the cabinet.

## Test start and end timing

The scheduled start is unchanged. The scan begins at planned duration minus `N * 2300 ms`: 300 ms relay gap, 1000 ms settling, 500 ms meter polling budget and 500 ms relay transport budget for each enabled line. Minimum duration is `ceil((60000 + N * 2300) / 1000)` seconds; it ensures at least 60 seconds of common warm-up. A requested longer duration is preserved. Early scan completion disconnects the meter and holds the cabinet in test mode until the planned end. Slow Modbus responses can extend the scan beyond the estimated end; freshness and exclusive selection take priority over cutting it short. Single-line test duration remains operator-controlled.

The 1000 ms settling interval is a commissioning assumption, not a manufacturer-specified DJSF1352 conversion time. Verify it on the cabinet by alternating shunts with different currents.
