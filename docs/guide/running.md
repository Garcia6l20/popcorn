# Running and stopping

A machine runs only inside `run`, `step` or `call`. Each one ends with a `popcorn.Stop` that
says why and where the core stopped. The examples on this page use the gpio example firmware:
a button on PC13 counts presses in `presses` through an EXTI interrupt, and `main` sleeps in WFI.

## Run until an address

```python
--8<-- "guide/test_running.py:until"
```

`until` takes a symbol or an address. The run stops with ADDRESS before the instruction there
runs, so `presses` is still 0 when the callback is reached. A run that starts at that address
executes at least one instruction first, so calling `run(until=...)` again goes on to the next
hit, as a debugger's continue does.

## Run for a time

```python
--8<-- "guide/test_running.py:for"
```

`for_` bounds the run in virtual time and stops it with TIME. A core asleep in WFI with nothing
due sleeps to the deadline, so a sleeping firmware reaches it at once. The stop lands on the
first block boundary at or after the deadline, so `time` can pass it by less than one
translated block.

## Step

```python
--8<-- "guide/test_running.py:step"
```

`step(n)` executes exactly `n` instructions and stops with BUDGET, or stops earlier for another
reason.

## Budgets count instructions, not sleep

```python
--8<-- "guide/test_running.py:budget"
```

`budget` caps the instructions a run executes, then stops it with BUDGET. Time the core spends
asleep in WFI does not count. A firmware that sleeps between interrupts can therefore run for a
long virtual time inside a small budget: here 1,000 instructions take more than 10 ms. Bound
time with `for_`, and keep `budget` as a guard against a firmware stuck in a busy loop.

Without `budget=`, a run uses the machine's `default_budget`, None for no limit. The pytest
plugin sets it per test, see [Testing with pytest](testing.md).

## Read a stop

```python
--8<-- "guide/test_running.py:stop"
```

`print(stop)` gives the reason and the source location, such as
`TIME at 0x0800036e main+0x86 examples/stm32u575/gpio/firmware/main.c:43`. The fields are:

- `reason`, a `popcorn.StopReason`.
- `pc`, the next instruction to execute, and `location`, its symbol, offset, file and line.
- `instructions`, the count executed since reset. Time asleep in WFI is not in it.
- `time`, the virtual time since reset.
- `fault`, set only for a FAULT stop. See [Faults and errors](faults.md).
- `detail`, what caused the stop beyond its reason.

## Stop reasons

| Reason | Cause |
|---|---|
| `ADDRESS` | `until` was reached. |
| `TIME` | `for_` elapsed. |
| `BUDGET` | The instruction budget ran out, or `step(n)` executed its `n` instructions. |
| `IDLE` | The core sleeps in WFI with nothing that can wake it and no `for_` deadline. |
| `RETURNED` | The function a `call` or `run(until=popcorn.FrameReturn())` waits on returned. |
| `FAULT` | A processor fault under `faults="stop"`. |
| `RESET_REQUEST` | The firmware requested a system reset. |
| `SPIN` | A loop made no progress for `spin_threshold` iterations. |
| `WATCHPOINT` | A watch on a memory access stopped the run. |
| `USER` | A `stop_when` predicate returned True, a handler stopped the run, or a BKPT ran. |
