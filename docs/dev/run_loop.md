# Run loop

A machine runs in slices. A slice executes translated blocks until a bound ends it, then
returns to its caller, which yields to Python and calls again. The loop is
`machine::run_state::run` in `native/src/core/run_loop.cpp`.

## Time

The virtual clock counts executed instructions. Virtual time is piecewise linear: each change
of the core rate starts an epoch, and time inside an epoch is the instructions since its start
divided by the instruction rate, `core_hz / cpi`. The clock never reads wall time. The
instruction counter advances once per translated block, so it is exact to the block.

A core asleep in WFI executes nothing. When nothing is deliverable the loop jumps the clock to
the next deadline, which is why a sleeping firmware runs fast. The slice keeps two counters,
`executed` and `jumped`. A run's `budget` counts `executed` only, and `max_instructions`
counts both.

A core in a branch to itself, a one-instruction block that `arch::self_branch` names, such as
Arm `b .` outside an IT block, executes instructions. When the last block hook saw the same
one-instruction block, the arch decodes it from its bytes and the core state. When nothing is
deliverable the hook then advances the clock to the deadline or the next
event, whichever comes first, then stops the engine before the block. The skipped iterations
count as executed, as if they had run, so every output equals the slow path. The skip is off
while a per-block observer is on: stepping, coverage, block trace records, spin detection, a
return watch, or a code hook site on the block. `self_branch_skips` in the run counters counts
the skips.

## Events

Models schedule events on the scheduler. It orders them by due count, then lane, then sequence
number. A deadline counts either executed instructions or virtual time. The loop dispatches
every event due at the current count before it runs the engine, and it never runs the engine
past the earliest pending deadline.

## One turn of the loop

```mermaid
flowchart TD
    A[handle requests<br/>stop, reset, slice end] --> B[dispatch due events]
    B --> C[handle requests]
    C --> D{bound reached?}
    D -->|yes| Z[finish the slice]
    D -->|no| E{core held?}
    E -->|yes| F[jump the clock to the<br/>next deadline] --> A
    E -->|no| G[deliver a pending exception]
    G --> H{core asleep?}
    H -->|yes| F
    H -->|no| I[set the instruction deadline]
    I --> J[run the engine]
    J --> K[count executed instructions]
    K --> L[handle requests and traps]
    L --> A
```

In order, one turn of the loop:

1. Handles requests. A model or handler may have asked for a stop, a reset or the end of the
   slice. A requested reset becomes a RESET_REQUEST stop when a call is running or the machine
   is configured to stop on it. A run that waits for thread mode stops the same way. Otherwise the machine resets and the run goes on.
   A RESET_REQUEST stop keeps its kind as `reset_pending` until a reset or a restore, saved in
   snapshots. The Python binding applies it with `machine::apply_reset_request` before the next
   run, step or free run of a lone machine starts, so `for_` counts from the reset. A call never
   applies it. A system handles its members' requests itself.
2. Dispatches the events due at the current instruction count.
3. Checks the bounds, in this order: the call return sentinel, `budget_left`,
   `exact_instructions`, `until_time`, the `until` addresses, `until_return`, and last
   `max_instructions`, which ends the slice without a stop.
4. When the core is held, as a halted member of a system is, or the slice runs with `execute`
   off, jumps the clock to the earliest of the next event, the time bound and the instruction
   bound. With no event and no time bound it stops
   with IDLE.
5. Otherwise delivers a pending exception. A pending interrupt therefore wakes a core in WFI
   before any jump. If the core still sleeps after the delivery, it jumps the clock as in step 4.
6. Otherwise takes the smallest of the instruction bounds and the next event as the deadline and
   starts the engine with `uc_emu_start`.
7. After the engine returns, adds the executed instructions, handles requests, then turns a bus
   fault or an unmapped access into a stop or a resume through the architecture module.

The engine stops at the deadline, at an address of the exit list, or when a hook asks it to.
The exit list holds the `until` addresses and the call return sentinel. A slice that starts at
one of them executes one instruction first when `leave_until` is set, so a repeated run goes on
to the next hit.

## Bounds of a slice

`slice_request` in `native/include/popcorn/core/machine.hpp` carries the bounds. The bounds
`until`, `budget_left`, `exact_instructions`, `until_time` and `stop_when` stop the run. The
instruction cap of one million and the wall budget of one millisecond, read every 256 blocks,
only end the slice so the caller can yield. `exact_instructions` runs whole blocks up to the
last block boundary before the count, then single-steps the rest. `stop_when` is evaluated
after each dispatched event and at the slice end, never per instruction.

## Finishing

`finish` turns the end into a `stop` record: the reason, the program counter, the instruction
count, the virtual time, the reset count and any fault. A requested stop wins over a
`stop_when` stop at the same boundary. For a stop the caller sees, `finish` locates the program
counter in the images and writes a stop record to the trace.

Stop reasons are the `stop_reason` enum in `native/include/popcorn/core/types.hpp`. The Python
`StopReason` mirrors it.
