# Systems

A `core::system` runs several machines on one virtual clock and links their UARTs. The type is
declared in `native/include/popcorn/core/system.hpp`. The Python `System` wraps it.

## Members and hosts

Each member is a machine served by its own host, which owns a native thread, see
[Threads](threads.md). The system links the hosts of its members with `member_link` wires. A
wire is one direction of a link. It has a sender, a receiver and a latency `L` above zero.

## Windows and the barrier

The members meet at a `window_barrier` every quantum `Q` of system time. `Q` is the smallest
latency of the system, and the barriers fall at the system times `k * Q` from the start.

```mermaid
sequenceDiagram
    participant S as sender member
    participant B as barrier
    participant R as receiver member
    S->>S: run window k, post frames to the mailbox
    S->>B: arrive
    R->>B: arrive
    B->>B: last arriver runs the completion
    B-->>R: release
    R->>R: drain the wires, senders in member order, then link order
    R->>R: run window k + 1
```

- A member whose core runs is held at each barrier with `slice_request::hold_from_until`, so
  every post reaches its receiver at its exact time whatever `Q`, the slicing or the thread
  timing.
- A sender appends a frame to the mailbox of its current window. After barrier `k` the
  receiver drains the mailbox of window `k` on its own thread. The sender writes that mailbox
  again only in window `k + 2`, which holds only while every member arrives at every barrier.
- A frame that ends at sender time `t` is delivered at `t + L`.
- `member_link::sender_reset` and `receiver_reset` tell a link that a member reset. A frame in
  flight at a sender reset is aborted, and a receiver reset drops the frames whose arrival is
  still on the line, so the dropped set depends on the reset time and `L` only.

The barrier is `window_barrier` in `native/src/core/window_barrier.cpp`. The last party to
arrive runs a completion, then bumps a generation counter and wakes the sleepers. Waiters spin
for the wait policy's time, then sleep on a futex. When there are more parties than usable
CPUs, `default_wait_policy` sets the spin to zero.

## Halted members

A member with no goal while another member runs one is halted. Its core is held and it advances
to the system's decision after each barrier, so its clocks, peripherals and DMA follow system
time. A halted member's time counts in its instruction count but not in a goal's budget.

## Locks

The system has one mutex. Only threads that are not host threads take it, and a thread holding
it takes at most one host mutex at a time. Host threads never take the system mutex.

## Determinism

The frames, their times and every member's state do not depend on `Q`, on the slicing or on
the thread timing. The barrier and the mailbox rules above give that property, and the
linked_uart example tests it at several quanta, slicings and CPU counts.
