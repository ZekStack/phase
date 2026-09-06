# Examples

The repository includes Arduino examples under `examples/`.

| Example | Demonstrates |
| --- | --- |
| `Basic` | Minimal async boot with init/deinit and ready callback. |
| `Dependencies` | Dependency order, start/stop callbacks, and shutdown. |
| `Groups` | Readiness groups and condition polling. |
| `PauseResume` | Pause before boot and resume from `loop()`. |
| `OptionalNodes` | Optional failure and skipped optional dependents. |
| `BindableCallbacks` | Calling private methods through lambdas. |
| `ManualShutdown` | Requesting shutdown after ready. |
| `MemoryPolicy` | Configuring Strata graph/task placement and inspecting actual stack region. |

## Recommended order

Start with `Basic`, then read `Dependencies` and `Groups`.

Use `MemoryPolicy` when integrating Phase into a larger Strata-backed application. It shows both `memory.allocation` and `memory.taskStack`, plus the requested-placement/actual-region diagnostic split.

`PauseResume` and `OptionalNodes` cover behavior that matters in larger products.

`BindableCallbacks` shows how to keep module internals private while still registering lifecycle callbacks.
