# Getting Started

Phase runs application lifecycle work from its own FreeRTOS task.

The usual flow is:

```txt
phase.init()
phase.add(...)
phase.addGroup(...)
phase.start()
```

`start()` returns immediately. Boot continues inside the Phase task.

## Minimal setup

```cpp
#include <Arduino.h>
#include <Phase.h>

Phase phase;

void setup() {
	Serial.begin(115200);

	PhaseResult result = phase.init();
	if (!result) {
		Serial.println(result.message);
		return;
	}

	phase.add("storage", []() {
		Serial.println("storage initialized");
	});

	phase.onReady([]() {
		Serial.println("ready");
	});

	phase.start();
	Serial.println("setup is not blocked");
}

void loop() {
	delay(1000);
}
```

## Steps

A step is a real module. It can have:

```txt
init
deinit
start
stop
```

Only `init` is required.

```cpp
phase.add("network", initNetwork, deinitNetwork)
    .start(startNetwork, stopNetwork);
```

Use `init` to allocate, configure, and register handlers. Use `start` to activate work such as connections, listeners, tasks, or clients.

## Dependencies

Dependencies define lifecycle order.

```cpp
phase.add("database", initDatabase, deinitDatabase)
    .depends({ "storage", "time" });
```

During the init wave, step nodes initialize after their step dependencies have initialized. Groups are not evaluated in the init wave. During the start/readiness wave, steps start and groups are evaluated only after their dependencies are ready.

## Groups

A group is a virtual readiness gate. It does not own resources.

```cpp
phase.addGroup("internet")
    .depends("network")
    .condition([]() {
        return networkHasInternet();
    }, 30000);
```

Groups are useful for readiness states such as link-up, internet, configured, or cloud-authenticated.

Groups are evaluated during the start/readiness wave, so a group such as `internet` can safely depend on a started `network` step instead of only an initialized one.

## Pause and resume

Pause is for external lifecycle control.

```cpp
phase.pause("factory-reset");
phase.start();

// Later:
phase.resume();
```

Pause is cooperative. It takes effect before the next lifecycle action or the next group-condition poll.

## Shutdown

`stop()` requests reverse shutdown:

```txt
started steps stop in reverse start order
initialized steps deinitialize in reverse init order
groups reset internally
```

```cpp
phase.stop();
```

Calling `stop()` while Phase is idle, stopped, or failed is a success no-op. If `pause()` was called before `start()`, a pre-start `stop()` does not clear that pause; the later `start()` still waits for `resume()`.

`end()` is final teardown. After `end()` succeeds, create a new `Phase` object instead of reusing the same instance.
