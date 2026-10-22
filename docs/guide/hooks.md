# Hooks, probes and calls

Hooks watch or change what the firmware does without editing it. The examples on this page use
the gpio firmware, where a press on PC13 runs `HAL_GPIO_EXTI_Falling_Callback`, which counts it
in `presses` and toggles the LED. Run the machine for a millisecond first, so the firmware has
configured the pin.

## Intercept a function

```python
--8<-- "guide/test_hooks.py:intercept"
```

`machine.intercept(fn, handler)` stops each call of `fn` at its first instruction and runs
`handler(call)`. `fn` is a symbol name, a `Symbol` or an address. Without a handler the call is a
decorator. The handler returns one of:

- `call.proceed()` runs the function.
- `call.returns(value)` skips the function and returns `value`. `None` returns from a void
  function.
- `call.stop(reason)` ends the run.

`call.arg(i)` and `call.args_typed()` read the arguments, typed from DWARF. The handler runs
holding the GIL, may read the machine, and must not await. An exception it raises reaches the
run with its traceback. `once=True` removes the intercept after its first call.

`machine.intercept_log(fn)` lists every call the intercepts of `fn` saw, with its arguments and the
value it returned.

```python
--8<-- "guide/test_hooks.py:replace"
```

Skipping `HAL_GPIO_TogglePin` leaves the LED off while the firmware still counts the press.

`machine.intercepted(fn, handler)` is a context manager for an intercept that lives for one block.
`intercept.remove()` and `machine.remove_hooks()` remove them. The pytest plugin removes the hooks of a
test before the next one starts.

### Scripted answers

```python
--8<-- "guide/test_hooks.py:script"
```

`machine.script(fn, replies)` answers the first calls of `fn` with `replies` in order, then runs
the original.

## Return hooks

```python
--8<-- "guide/test_hooks.py:return"
```

`machine.on_return(fn, handler)` calls `handler(returned)` each time `fn` returns. A `Returned`
holds the entry arguments, the return value and the instructions the call took. The function needs a
DWARF signature.

`machine.record_returns(fn, convert)` records each return with its virtual time and no GIL
without `convert`:

```python
--8<-- "guide/test_hooks.py:record"
```

`recording.mark()` marks a point and `recording.since(mark)` returns the `Recorded` entries
after it, each with `at` and `value`.

## Watch memory

```python
--8<-- "guide/test_hooks.py:watch"
```

`machine.watch(target, size, kind)` watches `size` bytes at an address or symbol for reads,
writes or both. It sees accesses by the CPU and by DMA. Each hit is a `WatchHit` with the `pc`,
the `address`, the `value`, whether it was a `write` and the `initiator`. `watch.remove()` ends it.

With no handler every hit stops the run:

```python
--8<-- "guide/test_hooks.py:watch_stop"
```

A handler returns `WatchAction.STOP` or `WatchAction.RESUME`. `None` means resume. A handler runs
holding the GIL, so prefer a probe or a recording when you only count.

## Probe an address

```python
--8<-- "guide/test_hooks.py:probe"
```

`machine.probe(target)` counts the core's arrivals at an address natively. It takes no handler and
never takes the GIL, so it costs nothing on the machine thread. `probe.count` and
`probe.first_at` read the result at any time. `await probe.hit(within=)` waits for the first
arrival. A probe never stops the run.

With `state="secure"` or `"nonsecure"` it counts the arrivals in that TrustZone state only.

## Wait for a condition

```python
--8<-- "guide/test_hooks.py:condition"
```

A `Condition` compares one plain-memory value with a constant: `machine.ref.var("presses").eq(1)`
and its siblings `ne`, `lt`, `le`, `gt` and `ge`. `machine.wait_for(condition, for_=)` runs until
it holds, with a USER stop, or until `for_` elapses, with a TIME stop. `popcorn.any_of(...)` and
`popcorn.all_of(...)` combine conditions, and `stop.fired` lists the members that held.

In a free run, `async with machine.running()`, a condition is evaluated natively with no Python.

For anything a condition cannot say, pass a predicate:

```python
--8<-- "guide/test_hooks.py:predicate"
```

`wait_until(predicate, for_=)` returns the virtual time at which `predicate(machine)` first held,
or `None`. The predicate is sync and runs at event boundaries and slice ends, never per
instruction.

## Call a firmware function

```python
--8<-- "guide/test_hooks.py:call"
```

`await machine.call(fn, *args)` runs a firmware function on the machine and returns a
`CallResult`. `result.value` is the return value and `result.stop` how the call ended. The
argument count and types come from DWARF when the image has them. When the call returns, the
registers and the execution context come back as they were. Writes to memory and to the
peripherals stay.

`reset=True` or `reset=snapshot` restores a state first. `budget=` bounds the call. An exception
a handler or a model raises during the call ends it and restores the context the same way.

## Which one to use

| Need | Use |
|---|---|
| Know the arguments of a call | `intercept` and `intercept_log` |
| Change a result or skip a function | `intercept` with `returns` |
| Count calls or find when one happened | `probe` |
| Record results with times | `record_returns` |
| Catch a write by the CPU or by DMA | `watch` |
| Wait for a variable | `wait_for` with a `Condition` |
| Run firmware code from the test | `call` |
