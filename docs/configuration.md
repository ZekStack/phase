# Configuration

`PhaseConfig` controls memory placement, the Phase task, lifecycle timeouts, polling, and registration limits.

```cpp
PhaseConfig config;
config.memory.allocation = Strata::Placement::Default;
config.memory.taskStack = Strata::Placement::PreferExternal;
config.stackSizeBytes = 4096;
config.priority = 1;
config.coreId = tskNO_AFFINITY;
config.taskName = "phase-task";

PhaseResult result = phase.init(config);
```

## Memory policy

Phase v0.2.0 uses `Strata::MemoryPolicy`, the same memory-policy shape used across Strata-backed ZekStack libraries.

| Field | Default | Purpose |
| --- | --- | --- |
| `memory.allocation` | `Default` | Placement for movable Phase-owned graph storage. |
| `memory.taskStack` | `PreferExternal` | Placement for the Phase task stack. |

`memory.allocation` controls node records, node and dependency names, dependency indexes, init/start order storage, and graph validation storage.

`memory.taskStack` controls the Phase task stack. The task control block and recursive mutex control storage remain internal through Strata.

Available placements are:

* `Strata::Placement::Default` - use the Strata backend default.
* `Strata::Placement::Internal` - require internal memory.
* `Strata::Placement::PreferExternal` - prefer external memory and fall back to internal memory.
* `Strata::Placement::RequireExternal` - require external memory and fail when it is unavailable.

The default task placement preserves the old `PhaseStackType::Auto` behavior.

```cpp
PhaseDiag diag = phase.getDiagnostics();
Serial.printf(
    "requested=%s actual=%s\n",
    Strata::toString(diag.requestedStackPlacement),
    Strata::toString(diag.stackRegion)
);
```

The requested placement and actual region are intentionally separate because `PreferExternal` may fall back to internal memory.

## Task options

| Field | Default | Purpose |
| --- | --- | --- |
| `stackSizeBytes` | `4096` | FreeRTOS stack size in bytes. |
| `priority` | `1` | Phase task priority. |
| `coreId` | `tskNO_AFFINITY` | Core affinity. |
| `taskName` | `"phase-task"` | FreeRTOS task name. |

Strata owns the Phase task stack and task control block. Phase no longer contains ESP-IDF heap-capability or task-allocation logic.

## Limits

| Field | Default | Purpose |
| --- | --- | --- |
| `maxNodes` | `32` | Maximum total steps and groups. |
| `maxDependenciesPerNode` | `8` | Maximum dependencies for one node. |

These limits bound how many nodes and dependencies Phase accepts. Phase pre-reserves graph backing from these limits during initialization/registration so lifecycle execution can remain allocation-free.

## Timeouts

| Field | Default | Purpose |
| --- | --- | --- |
| `defaultInitTimeoutMs` | `30000` | Default init callback timeout. |
| `defaultStartTimeoutMs` | `30000` | Default start callback timeout. |
| `defaultStopTimeoutMs` | `30000` | Default stop callback timeout. |
| `defaultDeinitTimeoutMs` | `30000` | Default deinit callback timeout. |
| `defaultGroupTimeoutMs` | `30000` | Default group condition timeout. |
| `conditionPollIntervalMs` | `100` | Default group condition poll interval. |

Lifecycle callback timeouts are cooperative. Phase measures elapsed time after a callback returns. A callback that never returns cannot be interrupted by Phase.

Because Strata owns the static task storage, final task cleanup must happen from another task context. `end()` therefore returns `Busy` when called from the Phase task. A `Phase` object must not be destroyed from one of its own callbacks.

Calling `end()` performs final teardown for the current `Phase` instance. A successfully ended instance cannot be initialized again.

Group condition polling timeouts are controlled by Phase and pause cleanly while Phase is paused.

## Per-step timeouts

```cpp
phase.add("network", initNetwork, deinitNetwork)
    .start(startNetwork, stopNetwork)
    .initTimeout(10000)
    .startTimeout(60000)
    .stopTimeout(10000)
    .deinitTimeout(10000);
```

## Per-group polling

```cpp
phase.addGroup("internet")
    .depends("network")
    .condition(hasInternet, 30000)
    .conditionPollInterval(250);
```

## Migrating from v0.1.x

| v0.1.x | v0.2.0 |
| --- | --- |
| `PhaseStackType::Auto` | `Strata::Placement::PreferExternal` |
| `PhaseStackType::Internal` | `Strata::Placement::Internal` |
| `PhaseStackType::Psram` | `Strata::Placement::RequireExternal` |
| `config.stackType` | `config.memory.taskStack` |
| `diag.requestedStackType` | `diag.requestedStackPlacement` |
| `diag.actualStackType` | `diag.stackRegion` |
