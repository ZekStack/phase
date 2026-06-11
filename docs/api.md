# API Reference

This page summarizes the public API declared in `src/Phase.h`.

## Results

Phase does not use exceptions. Operations return `PhaseResult`.

| Field | Meaning |
| --- | --- |
| `result` | `true` on success, `false` on failure. |
| `status` | Machine-readable `PhaseStatus`. |
| `message` | Human-readable status. |

`PhaseStatus` values include `Ok`, `NotInitialized`, `AlreadyInitialized`, `InvalidArgument`, `OutOfMemory`, `TaskCreateFailed`, `TooManyNodes`, `TooManyDependencies`, `DuplicateName`, `MissingDependency`, `CircularDependency`, `InvalidCallback`, `RegistrationClosed`, `Busy`, `Timeout`, `CallbackFailed`, `DependencyFailed`, and `InternalError`.

## Phase

| Method | Purpose |
| --- | --- |
| `init(config)` | Create the Phase task and prepare registration. |
| `start()` | Request async boot. |
| `stop()` | Request reverse stop/deinit. |
| `end(timeoutMs)` | Stop the task and permanently end this Phase instance. |
| `pause(reason)` | Request cooperative pause. |
| `resume()` | Continue lifecycle progression. |
| `isPaused()` | Return current pause flag. |
| `state()` | Return current lifecycle state. |
| `getDiagnostics()` | Return aggregate diagnostics. |
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

```txt
void
bool
PhaseResult
```

`false` maps to `PhaseStatus::CallbackFailed`.

Group conditions return `bool`.

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

## Diagnostics

`PhaseDiag` reports node counts, boot count, rollback count, change count, state, stack memory preference, and the task stack high-water mark after the task ends.

`startedCount` counts only steps whose `start()` callback completed. Steps without a `start()` callback can become ready without being counted as started.

## Stop and end behavior

`stop()` is a no-op when Phase is idle, stopped, or failed. It requests shutdown when Phase is booting, starting, ready, or actively paused.

`end()` is final teardown for a Phase instance. After `end()` succeeds, create a new `Phase` object instead of calling `init()` again on the same object.
