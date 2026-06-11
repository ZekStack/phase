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

## Recommended order

Start with `Basic`, then read `Dependencies` and `Groups`.

`PauseResume` and `OptionalNodes` cover behavior that matters in larger products.

`BindableCallbacks` shows how to keep module internals private while still registering lifecycle callbacks.
