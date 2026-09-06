# Phase

Phase is an async application lifecycle orchestration library for ESP32.

Phase helps you boot and shut down larger Arduino ESP32 applications in a predictable order. It owns lifecycle orchestration and dependency policy while [Strata](https://github.com/ZekStack/strata) owns Phase memory placement and low-level FreeRTOS storage.

[![CI](https://github.com/ZekStack/phase/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/phase/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/phase?sort=semver)](https://github.com/ZekStack/phase/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Phase?

* **Async boot** - `start()` wakes the Phase task and returns immediately.
* **Dependency order** - steps and groups declare what must be ready first.
* **Two-layer lifecycle** - simple modules use init/deinit, advanced modules add start/stop.
* **Readiness groups** - wait for virtual gates such as network link or internet access.
* **Consistent memory policy** - `Strata::MemoryPolicy` controls graph allocations and task-stack placement.
* **Strata-owned FreeRTOS storage** - the Phase task stack, task control block, and recursive mutex storage are owned by Strata.
* **Production-minded** - thread-safe internals, rollback, diagnostics, progress callbacks, and allocation-free lifecycle execution after registration.

## Dependency

Phase `v0.2.0` requires Strata `v0.1.1`.

### PlatformIO

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
  https://github.com/ZekStack/phase.git#v0.2.0

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

Phase's `library.json` pins Strata `v0.1.1`, so PlatformIO resolves it transitively.

### Arduino IDE

Phase and Strata are not published to Arduino Library Manager yet. Install both repositories into the Arduino libraries directory:

```text
Arduino/libraries/Strata
Arduino/libraries/Phase
```

Use Strata `v0.1.1` or a compatible later release.

## Quick start

```cpp
#include <Arduino.h>
#include <Phase.h>

Phase phase;

void setup() {
    Serial.begin(115200);

    PhaseResult initResult = phase.init();
    if (!initResult) {
        Serial.println(initResult.message);
        return;
    }

    phase.add("storage", []() {
        Serial.println("storage init");
    });

    phase.add("network", []() {
        Serial.println("network init");
    }).start([]() {
        Serial.println("network start");
    });

    phase.onReady([]() {
        Serial.println("app ready");
    });

    phase.start();
    Serial.println("setup continues while Phase boots");
}

void loop() {
    delay(1000);
}
```

## Memory policy

Phase uses the ZekStack-standard Strata configuration shape:

```cpp
PhaseConfig config;
config.memory.allocation = Strata::Placement::PreferExternal;
config.memory.taskStack = Strata::Placement::PreferExternal;

PhaseResult result = phase.init(config);
```

`memory.allocation` controls movable Phase-owned graph storage: node records, node/dependency names, dependency indexes, lifecycle order, and validation backing.

`memory.taskStack` controls the Phase task stack. Task control-block and mutex control storage remain internal through Strata.

The default policy preserves Phase v0.1.0 behavior:

```cpp
allocation = Strata::Placement::Default;
taskStack  = Strata::Placement::PreferExternal;
```

`PreferExternal` falls back to internal memory when external memory is unavailable. `RequireExternal` fails task creation rather than consuming internal memory.

Diagnostics report requested placement separately from the observed memory region:

```cpp
PhaseDiag diag = phase.getDiagnostics();
Serial.printf(
    "requested=%s actual=%s\n",
    Strata::toString(diag.requestedStackPlacement),
    Strata::toString(diag.stackRegion)
);
```

## Important notes

> [!IMPORTANT]
> Phase callbacks are cooperative. Lifecycle callbacks run inside the Phase task, so a callback that never returns cannot be interrupted by Phase.

* `pause()` is async and takes effect before the next lifecycle action or group-condition poll.
* Callback timeout checks happen after the callback returns.
* Group condition polling timeouts are enforced by the Phase task.
* Registration closes after a successful `start()` request.
* `stop()`, `pause()`, and `resume()` may be called from Phase callbacks. `end()` must be called from another task and returns `Busy` when called from the Phase task.
* A `Phase` object must not be destroyed from one of its own Phase callbacks. Strata task storage can only be reclaimed safely from another task context.
* Graph storage is allocated during initialization/registration and pre-reserved from the configured limits. Lifecycle execution remains allocation-free.
* Callback callables remain `std::function`; allocations performed internally by an arbitrary callable representation are outside Phase's placement contract.
* `PhaseChange` string pointers are valid for the complete callback invocation. Event messages and pause reasons are copied into bounded internal snapshots and may be truncated to 191 characters.
* Stop/deinit failures are best-effort and are reported through `onChange()` while remaining cleanup continues.
* Phase depends only on Strata within the ZekStack library ecosystem.

## Migrating from v0.1.x

Phase `v0.2.0` removes `PhaseStackType` and `PhaseConfig::stackType`.

| v0.1.x | v0.2.0 |
| --- | --- |
| `PhaseStackType::Auto` | `Strata::Placement::PreferExternal` |
| `PhaseStackType::Internal` | `Strata::Placement::Internal` |
| `PhaseStackType::Psram` | `Strata::Placement::RequireExternal` |
| `diag.requestedStackType` | `diag.requestedStackPlacement` |
| `diag.actualStackType` | `diag.stackRegion` |

For the old default behavior, no configuration change is required: the v0.2.0 default task placement is already `PreferExternal`.

## Examples

| Example | Description |
| --- | --- |
| `Basic` | Minimal async boot with init/deinit and ready callback. |
| `Dependencies` | Dependency order, start/stop, and shutdown order. |
| `Groups` | Virtual readiness groups with condition polling. |
| `PauseResume` | Pause before boot and resume from `loop()`. |
| `OptionalNodes` | Optional node failure and skipped dependent behavior. |
| `BindableCallbacks` | Bind private class methods with lambdas. |
| `ManualShutdown` | Request reverse stop/deinit from `loop()`. |
| `MemoryPolicy` | Configure Strata graph/task placement and inspect diagnostics. |

## Documentation

Detailed documentation is available in the `docs/` folder.

| Document | Description |
| --- | --- |
| [`docs/getting-started.md`](docs/getting-started.md) | Step-by-step setup and first lifecycle flow. |
| [`docs/configuration.md`](docs/configuration.md) | Memory, task, timeout, limit, and polling options. |
| [`docs/api.md`](docs/api.md) | Public classes, methods, callbacks, and result types. |
| [`docs/examples.md`](docs/examples.md) | Explanation of all included examples. |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | Common issues and behavior notes. |

## API overview

```cpp
Phase phase;
phase.init();
phase.add("storage", initStorage, deinitStorage);
phase.add("network", initNetwork, deinitNetwork).start(startNetwork, stopNetwork);
phase.addGroup("internet").depends("network").condition(hasInternet, 30000);
phase.onReady([]() {});
phase.onFailed([](PhaseResult result) {});
phase.start();
```

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Platform | `espressif32` |
| Language | C++20 |
| Filesystem | none |
| PSRAM | Optional; controlled through Strata placement |
| Dependencies | Strata `v0.1.1` |
| Exceptions | Not intentionally used by Phase |
| Status | `0.2.0` |

## Error handling

Phase reports operation status through `PhaseResult`.

```cpp
PhaseResult result = phase.start();

if (!result) {
    Serial.println(result.message);
    return;
}
```

For all error codes, see [`docs/api.md`](docs/api.md).

## License

MIT - see [`LICENSE.md`](LICENSE.md).

## ZekStack

Part of the ZekStack ESP32 library stack.
