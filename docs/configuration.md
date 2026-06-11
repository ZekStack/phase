# Configuration

`PhaseConfig` controls the task, lifecycle timeouts, polling, and registration limits.

```cpp
PhaseConfig config;
config.stackSizeBytes = 4096;
config.priority = 1;
config.coreId = tskNO_AFFINITY;
config.taskName = "phase-task";
config.stackType = PhaseStackType::Auto;

PhaseResult result = phase.init(config);
```

## Task options

| Field | Default | Purpose |
| --- | --- | --- |
| `stackSizeBytes` | `4096` | FreeRTOS stack size in bytes. |
| `priority` | `1` | Phase task priority. |
| `coreId` | `tskNO_AFFINITY` | Core affinity. |
| `taskName` | `"phase-task"` | FreeRTOS task name. |
| `stackType` | `Auto` | Internal RAM or PSRAM stack preference. |

`PhaseStackType::Auto` prefers PSRAM task stacks when the ESP-IDF support is available and falls back to internal RAM.

## Limits

| Field | Default | Purpose |
| --- | --- | --- |
| `maxNodes` | `32` | Maximum total steps and groups. |
| `maxDependenciesPerNode` | `8` | Maximum dependencies for one node. |

These limits bound how many nodes and dependencies Phase accepts. Registration still uses dynamic allocation internally through `std::vector`, `std::string`, and `std::function`, so register all nodes during setup and avoid runtime registration.

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

Because callbacks cannot be interrupted, destroying a `Phase` object waits indefinitely for the Phase task to exit. If a lifecycle callback never returns, the destructor can block forever.

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
