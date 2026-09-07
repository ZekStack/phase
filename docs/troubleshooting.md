# Troubleshooting

## `start()` succeeds but nothing boots

Check that Phase was initialized and at least one step was registered.

```cpp
PhaseResult result = phase.init();
```

If `pause()` was called before `start()`, boot waits until `resume()`.

## `init()` fails with `TaskCreateFailed`

Check the requested Strata task placement and available memory.

```cpp
PhaseConfig config;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

`PreferExternal` falls back to internal memory when external memory is unavailable. `RequireExternal` does not fall back; task creation fails instead.

If the device does not have usable external RAM, use `Internal` or `PreferExternal`.

## The task did not land where expected

Requested placement and actual region are separate diagnostics:

```cpp
PhaseDiag diag = phase.getDiagnostics();
Serial.printf(
    "requested=%s region=%s\n",
    Strata::toString(diag.requestedStackPlacement),
    Strata::toString(diag.stackRegion)
);
```

Seeing `Internal` after requesting `PreferExternal` is a valid fallback. Use `RequireExternal` only when failure is preferable to consuming internal RAM.

`allocationPlacement` reports the requested policy for Phase-owned graph storage; it is not a claim that every caller-owned lambda capture is placed there.

## Registration fails after start

Registration closes after `start()`.

The expected order is:

```text
init
add steps
add groups
start
```

## Boot fails before the first callback

Phase validates the graph before booting. Common causes:

* duplicate step/group names
* empty names
* missing dependencies
* circular dependencies
* too many nodes or dependencies

## A timeout does not stop a stuck callback

Lifecycle callbacks are cooperative. Phase can report that a returned callback exceeded its timeout, but it cannot interrupt a callback that never returns.

Use bounded waits inside callbacks and return a failed `PhaseResult` when the module cannot finish.

Phase destruction waits for the internal task to reach its Strata teardown handoff. If a callback never returns, destroying the `Phase` object can block forever.

`end()` must be called from another task context. It returns `Busy` from a Phase callback because the Strata-owned static task stack cannot safely free itself.

A `Phase` object must not be destroyed from one of its own callbacks.

`end()` is terminal. After it succeeds, create a new `Phase` object instead of calling `init()` again.

## A group timeout expires while waiting for a condition

Group condition polling is controlled by Phase. Increase the group timeout or make the condition represent a lower-level readiness gate.

```cpp
phase.addGroup("internet")
    .depends("network")
    .condition(hasInternet, 60000);
```

## Shutdown does not call a callback

Phase only stops successfully started steps and only deinitializes successfully initialized steps.

Groups do not stop or deinitialize because they do not own resources.

Steps without a `start()` callback are ready once their dependencies are ready, but they are not counted as started and do not run through the stop phase.
