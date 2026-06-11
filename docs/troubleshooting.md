# Troubleshooting

## `start()` succeeds but nothing boots

Check that Phase was initialized and at least one step was registered.

```cpp
PhaseResult result = phase.init();
```

If `pause()` was called before `start()`, boot waits until `resume()`.

## Registration fails after start

Registration closes after `start()`.

The expected order is:

```txt
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

Phase destruction waits for the internal task to exit. If a callback never returns, destroying the `Phase` object can block forever.

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
