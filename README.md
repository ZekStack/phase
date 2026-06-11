# Phase

Phase is an async application lifecycle orchestration library for ESP32.

Phase helps you boot and shut down larger Arduino ESP32 applications in a predictable order. It is designed for projects with multiple modules that depend on each other and focuses on dependency-ordered lifecycle steps, readiness gates, cooperative pause/resume, rollback, and result-based errors.

[![CI](https://github.com/ZekStack/phase/actions/workflows/ci.yml/badge.svg)](https://github.com/ZekStack/phase/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/ZekStack/phase?sort=semver)](https://github.com/ZekStack/phase/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE.md)

## Why use Phase?

* **Async boot** - `start()` wakes the Phase task and returns immediately.
* **Dependency order** - steps and groups declare what must be ready first.
* **Two-layer lifecycle** - simple modules use init/deinit, advanced modules add start/stop.
* **Readiness groups** - wait for virtual gates such as network link or internet access.
* **Production-minded** - thread-safe internals, no exceptions, rollback, diagnostics, and progress callbacks.

## Install

### PlatformIO

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino

lib_deps =
  https://github.com/ZekStack/phase.git

build_flags =
  -std=gnu++20
build_unflags =
  -std=gnu++11
```

### Arduino IDE

Phase is not published to Arduino Library Manager yet.

Install it by downloading the repository ZIP or cloning it into your Arduino libraries folder.

```txt
Arduino/libraries/Phase
```

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

## Important notes

> [!IMPORTANT]
> Phase callbacks are cooperative. Lifecycle callbacks run inside the Phase task, so a callback that never returns cannot be interrupted by Phase.

* `pause()` is async and takes effect before the next lifecycle action or group-condition poll.
* Callback timeout checks happen after the callback returns.
* Group condition polling timeouts are enforced by the Phase task.
* Registration closes after `start()`.
* Phase does not depend on other ZekStack libraries.

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

Start with:

```txt
examples/Basic
```

## Documentation

Detailed documentation is available in the `docs/` folder.

| Document | Description |
| --- | --- |
| [`docs/getting-started.md`](docs/getting-started.md) | Step-by-step setup and first lifecycle flow. |
| [`docs/configuration.md`](docs/configuration.md) | Task, timeout, limit, and polling options. |
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

For the full API, see [`docs/api.md`](docs/api.md).

## Compatibility

| Item | Support |
| --- | --- |
| Framework | Arduino ESP32 |
| Platform | `espressif32` |
| Language | C++20 |
| Filesystem | none |
| PSRAM | Optional for task stacks when ESP-IDF support is available |
| Dependencies | none |
| Exceptions | Not used |
| Status | Early-stage `0.0.1` |

## Configuration

```cpp
PhaseConfig config;
config.stackSizeBytes = 4096;
config.priority = 1;
config.coreId = tskNO_AFFINITY;
config.stackType = PhaseStackType::Auto;
config.defaultInitTimeoutMs = 30000;
config.conditionPollIntervalMs = 100;

PhaseResult result = phase.init(config);
```

For all options, see [`docs/configuration.md`](docs/configuration.md).

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
