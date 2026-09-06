# API Reference

This page summarizes the public API declared in `src/Phase.h` for Phase v0.2.0.

## Results

Phase does not intentionally throw exceptions. Operations return `PhaseResult`.

| Field | Meaning |
| --- | --- |
| `result` | `true` on success, `false` on failure. |
| `status` | Machine-readable `PhaseStatus`. |
| `message` | Human-readable status. |

`PhaseStatus` values include `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `OutOfMemory`, `TaskCreateFailed`, `TooManyNodes`, `TooManyDependencies`, `DuplicateName`, `MissingDependency`, `CircularDependency`, `InvalidCallback`, `RegistrationClosed`, `Busy`, `Timeout`, `CallbackFailed`, `DependencyFailed`, and `InternalError`.

## PhaseConfig

Phase v0.2.0 uses Strata for memory placement and owned FreeRTOS storage.

```cpp
PhaseConfig config;
config.memory.allocation = Strata::Placement::Default;
config.memory.taskStack = Strata::Placement::PreferExternal;
```

`memory.allocation` controls movable Phase-owned graph storage. `memory.taskStack` controls the Phase task stack. See `configuration.md` for the complete placement contract.

The old `PhaseStackType` API was removed in v0.2.0.

## Phase

| Method | Purpose |
| --- | --- |
| `init(config)` | Validate configuration, create Strata-backed storage/task ownership, and prepare registration. |
| `start()` | Request async boot. |
| `stop()` | Request reverse stop/deinit. |
| `end(timeoutMs)` | Stop the task, externally reclaim Strata task storage, and permanently end this Phase instance. |
| `pause(reason)` | Request cooperative pause. |
| `resume()` | Continue lifecycle progression. |
| `isPaused()` | Return current pause flag. |
| `state()` | Return current lifecycle state. |
| `getDiagnostics()` | Return aggregate and memory-placement diagnostics. |
| `onChange(callback)` | Register progress callback. |
| `onReady(callback)` | Register ready callback. |
| `onFailed(callback)` | Register terminal failure callback. |
| `add(name, init, deinit)` | Register a lifecycle step. |
| `addGroup(name)` | Register a virtual readiness group. |

## Step builder

`add()` returns `PhaseStepBuilder`.

| Method | Purpose |
| --- | --- |
| `depends(name)` | Add one dependency. |
| `depends({ ... })` | Add multiple dependencies. |
| `start(start, stop)` | Add optional start/stop callbacks. |
| `optional()` | Mark the step optional. |
| `initTimeout(ms)` | Override init timeout. |
| `startTimeout(ms)` | Override start timeout. |
| `stopTimeout(ms)` | Override stop timeout. |
| `deinitTimeout(ms)` | Override deinit timeout. |
| `result()` | Return the latest builder result. |

## Group builder

`addGroup()` returns `PhaseGroupBuilder`.

| Method | Purpose |
| --- | --- |
| `depends(name)` | Add one dependency. |
| `depends({ ... })` | Add multiple dependencies. |
| `condition(callback)` | Poll condition with default timeout. |
| `condition(callback, timeoutMs)` | Poll condition with override timeout. |
| `conditionPollInterval(ms)` | Override poll interval. |
| `optional()` | Mark the group optional. |
| `result()` | Return the latest builder result. |

## Callback return types

Lifecycle callbacks may return:

```text
void
bool
PhaseResult
```

`false` maps to `PhaseStatus::CallbackFailed`. Group conditions return `bool`.

## Change events

`PhaseChange` includes:

| Field | Meaning |
| --- | --- |
| `state` | Current lifecycle state. |
| `nodeType` | Step, group, or none. |
| `nodeName` | Current node name when available. |
| `pauseReason` | Pause reason when available. |
| `message` | Short event message. |
| `result` | Result associated with the change. |
| `durationMs` | Callback duration when available. |
| `hasError` | True when `result` failed. |

Callbacks are invoked synchronously from the Phase task. Keep them short.

`nodeName`, `pauseReason`, and `message` pointers are only guaranteed to be valid during the callback invocation. Copy them if you need to store them.

## Diagnostics

`PhaseDiag` reports lifecycle counts plus Strata placement information:

| Field | Meaning |
| --- | --- |
| `nodeCount` | Number of registered nodes. |
| `initializedCount` | Currently initialized steps. |
| `startedCount` | Steps whose start callback completed. |
| `readyCount` | Ready nodes. |
| `failedCount` | Failed nodes. |
| `skippedCount` | Skipped optional nodes. |
| `bootCount` | Number of boot attempts. |
| `rollbackCount` | Number of rollbacks. |
| `changeCount` | Number of emitted change events. |
| `stackHighWaterMarkBytes` | Phase task stack high-water mark captured during final teardown. |
| `state` | Current lifecycle state. |
| `requestedStackPlacement` | Requested `Strata::Placement` for the Phase task stack. |
| `stackRegion` | Observed `Strata::Region` for the task stack. |
| `allocationPlacement` | Requested placement for Phase-owned graph storage. |

Requested placement and observed region are separate because `PreferExternal` may fall back to internal memory.

## Stop and end behavior

`stop()` is a no-op when Phase is idle, stopped, or failed. It requests shutdown when Phase is booting, starting, ready, or actively paused. If `start()` was queued but the Phase task has not started booting yet, `stop()` cancels that pending start.

`end()` must run outside the Phase task. The Phase task performs lifecycle shutdown, records diagnostics, publishes an external-deletion handoff, and suspends. The caller then releases the Strata-owned static task stack and task control block.

`end()` returns `Busy` when called from a Phase callback. A `Phase` object must not be destroyed from one of its own Phase callbacks.

After `end()` succeeds, create a new `Phase` object instead of calling `init()` again on the same object.
