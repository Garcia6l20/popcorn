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
due sleeps to the deadline, so a sleeping firmware reaches it at once, on the first instruction
edge at or after it: up to one core period late, 250 ns at the 4 MHz MSI reset clock. A core
executing code stops on the first block boundary at or after the deadline, so `time` can pass
it by less than one translated block.

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
| `RESET_REQUEST` | The firmware requested a system reset. `detail` names its kind, such as `software` or `watchdog`. The next `run`, `step` or free run applies that reset first, unless `reset()` or `restore()` came between. A `call` leaves it pending. See `on_reset_request` below. |
| `SPIN` | A loop made no progress for `spin_threshold` iterations. |
| `WATCHPOINT` | A watch on a memory access stopped the run. |
| `USER` | A `stop_when` predicate returned True, a handler stopped the run, or a BKPT ran. |

`spin_threshold` stops a run with SPIN once a loop repeats the same pass that many times. The
loops analysis of `Machine.profile` reports such loops without stopping the run, and leaves the
SPIN stop of `spin_threshold` as it is. See [Profiling firmware](profiling.md).

### Reset requests

`Machine(on_reset_request="reset")` applies a requested reset and the run goes on, `Machine.resets`
counting each one. Only a `call` still stops with `RESET_REQUEST`. Under `"reset"`, `for_` and `time`
count from the latest reset, so a firmware that resets more often than the `for_` bound never ends the
run on `for_`. `wait_for` and `Reached` do not end when the loop is faster than an event that samples
them. Bound a run that may loop with `budget=` and watch `Machine.resets`. The option is for a lone
machine: a `System` member runs with `"stop"` and the system sets the reset with `on_member_reset`.

## Close a machine

`m.close()` ends a machine now and releases what it holds: its Python models, bus devices, hooks and handlers, its
host and its thread. Every later use of the machine raises `EmulationError`, `m.name` and `repr(m)` still work, and a
second `close()` does nothing. It refuses with `MachineRunning` while a run, call, wait or free run is active (`halt()`
first) and from a handler or a parked section, and with `ConfigError` for a member of a `System`.

```python
m = popcorn.Machine(description, image=image, models={"TIMER": Timer})
await m.run(for_=ms(1))
m.close()
```

`close()` halts and joins the machine's thread, then releases the machine, its models, devices, hooks and handlers at
once, even while a view, a `Pending`, a stream, a pin, a hook or a USB link made from the machine still exists. Every
later use of such an object that reaches the machine raises `EmulationError`, with the exception of what it holds
itself: a stream's frames already captured stay readable, and `Intercept.remove()` and `with m.intercepted(...)` exit
do nothing. A command whose awaiter was cancelled and that the close failed raises from `close()` after the release,
with a note naming the command. Further such failures go to `sys.unraisablehook`. `close()` is not supported while
another thread uses the same machine, as `del m` is not. A `Ctrl-C` during `close()` is delivered after it returns,
the wait for the thread to end cannot be interrupted.

At interpreter exit popcorn closes every machine your code still holds, after the machine threads stop. A Machine in
a module global with a Python model then leaves no `nanobind: leaked` report in the process that built it. An `atexit` handler registered after
`import popcorn` runs before that and uses its machines normally, a handler registered before the import finds them
closed. The machines of a `System` are left to their system.
