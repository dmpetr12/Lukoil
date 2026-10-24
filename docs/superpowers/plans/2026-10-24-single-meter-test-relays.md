# Single-meter Test Relays Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Safely sequence one shared meter across test lines while relay 1 removes normal voltage and all working line paths remain enabled.

**Architecture:** `TestController` owns warm-up and per-line measurement timing. `LineIoManager` owns the break-before-make relay invariant and emits complete module bytes through the existing Modbus path.

**Tech Stack:** C++17, Qt 6.10, Qt Test, CMake.

**Spec:** `docs/superpowers/specs/2026-10-24-single-meter-test-relays-design.md`

## Global Constraints

- Common relay 1 remains active throughout test mode.
- Working paths 3, 5, 7, ... remain enabled throughout test mode.
- At most one measurement path 4, 6, 8, ... may be active.
- Measurement relay changes use a 300 ms break-before-make interval.
- Each enabled line receives 10 seconds of measurement time near the end of the test.

## Review Focus

- Changing from one measured line to another must produce an all-measurement-off state first.
- Starting the first measurement must also observe the 300 ms relay gap.
- Stopping or fire during the gap must prevent the delayed relay from energizing.
- Lines on different relay modules must obey the same single-meter invariant.
- A single-line test must not disable its working path.

---

### Task 1: Test relay state machine

**Files:**
- Create: `tests/CMakeLists.txt`
- Create: `tests/line_io_manager_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `core/LineIoManager.h`
- Modify: `core/LineIoManager.cpp`

**Interfaces:**
- Consumes: existing relay mapping and `requestStepTestStart(int)` API.
- Produces: two-state break-before-make measurement switching with a 300 ms gap.

- [ ] Write failing Qt tests for common relay state, all working paths enabled, no measurement overlap, selected measurement connection, and cancellation during the gap.
- [ ] Run the new test executable and verify the old three-step behavior fails the assertions.
- [ ] Replace `TwoStepKind` with disconnected/connected measurement states and keep all working paths enabled.
- [ ] Run the relay tests and verify they pass.

### Task 2: Warm-up before first measurement

**Files:**
- Modify: `core/TestController.h`
- Modify: `core/TestController.cpp`
- Extend: `tests/line_io_manager_tests.cpp` or add a focused controller test if practical.

**Interfaces:**
- Consumes: `requestNoMeasTestStart()` and the updated `requestStepTestStart(int)`.
- Produces: warm-up with no meter connected, followed by 10-second per-line measurement steps.

- [ ] Add a failing test or deterministic state assertion proving the first line is not connected during warm-up.
- [ ] Split the warm-up timeout from the measurement-complete timeout.
- [ ] Include each 300 ms relay gap in the sequence timing calculation.
- [ ] Run all tests and a clean MinGW build with relevant warnings treated as errors.
- [ ] Commit the completed behavior.
