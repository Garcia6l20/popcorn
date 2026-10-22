# Running and stopping

A machine runs only inside `run`, `step` or `call`. Each one ends with a `popcorn.Stop` that
says why and where the core stopped. The examples on this page use the gpio example firmware:
a button on PC13 counts presses in `presses` through an EXTI interrupt, and `main` sleeps in WFI.
The snippets take a booted `button` machine:

```python
--8<-- "guide/test_running.py:describe"
```

`ELF` is the path of `gpio_button.elf`, see [First machine](first_machine.md). The button has a
pull-up, so the pin rests high and `level = 0` is a falling edge, the edge that fires the
interrupt. The first `run(for_=ms(1))` lets `main` configure the pin and reach WFI before the
test presses.

## Run until an address

```python
--8<-- "guide/test_running.py:until"
```

`until` takes a symbol or an address. The run stops with ADDRESS before the instruction there
runs, so `presses` is still 0 when the callback is reached. A run that starts at that address
executes at least one instruction first, so calling `run(until=...)` again goes on to the next
hit, as a debugger's continue does. Here the second call finds no new press and ends with TIME.

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

A firmware waiting in a branch to itself, `b .` or an empty `while(1)`, executes instructions,
and they count toward `budget`. While no exception can be taken, the run moves the clock over
the iterations to the next event or bound at once. Every output is the same as when they
execute one by one: the stop, `instructions`, `executed`, `time`, the order of events and the
counters the core reads, such as SysTick and DWT CYCCNT. A wait for a watchdog reset or a
timer interrupt in such a loop therefore costs little wall time. The loop executes each
iteration while coverage, trace block records, `spin_threshold`, a return watch, or an
intercept or probe on the branch is on. A loop of more than one instruction, such as a poll of
a flag, always executes.

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
- `instructions`, the count since reset. A core asleep in WFI adds one per core clock cycle.
  `Machine.executed` leaves that out and is what `budget` counts.
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
| `RESET_REQUEST` | The firmware requested a system reset. `detail` names its kind, such as `software` or `watchdog`. The next `run`, `step` or free run applies that reset first, unless `reset()` or `restore()` came between. A `call` leaves it pending. |
| `SPIN` | A loop made no progress for `spin_threshold` iterations. |
| `WATCHPOINT` | A watch on a memory access stopped the run. |
| `USER` | A `stop_when` predicate returned True, a handler stopped the run, or a BKPT ran. |
