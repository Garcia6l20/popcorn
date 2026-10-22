# Threads

popcorn runs each machine on its own native host thread. This page lists who may run where and
the lock rules that keep Python and the core from deadlocking.

## One host thread per machine

A machine lives in a `core::host`, declared in `native/include/popcorn/core/host.hpp`. The host
starts its thread at the first `run` or `start`, so a machine that is only accessed while parked
never gets one. A hosted machine is in one of four states:

- `parked`: no goal and no free run. Callers hold the host mutex and touch the machine directly.
- `running`: the host thread runs slices without the mutex. Callers post commands, served at the
  next block boundary.
- `idle_wait`: a free run idles with nothing due, and the thread sleeps until a request.
- `stopping`: the host closed. The machine stays reachable and nothing runs it.

The host thread takes its mutex only to change state and pop mail. It never holds it while it
calls into the machine or completes a future.

## Python on the machine thread

Python runs on the machine thread only for user callbacks: intercepts, return hooks, watches,
predicates, Python models and Python bus devices. The core itself takes no GIL. A Python model
takes the GIL on every call through its trampoline in `native/src/bind/model_trampoline.cpp`.

The helpers in `native/src/bind/gil_locks.hpp` keep the order of locks:

- `gil_entry` takes the GIL on entry to a callback. A thread that holds a link lock must not
  call it, because a link lock is a leaf. A host thread that runs no Python never takes the
  GIL, and the first entry creates its persistent Python thread state.
- `lock_releasing_gil` takes a host mutex without blocking on it while holding the GIL. On
  contention it releases the GIL, blocks, and takes the GIL back with the mutex held. If the
  wait would close a cycle of host mutex holders, it throws `machine_running`.
- `link_locked` runs a body under a link queue lock with the same rule: nobody waits on a link
  lock with the GIL held, and nobody takes the GIL under one.

The lock order is host mutex, then GIL, then leaf locks such as link locks. For systems the
system mutex comes first, see [Systems](systems.md). User code, a destructor included, never
runs under a host mutex.

## Thread sanitizer

The `tsan` variant builds the native tests with `-fsanitize=thread`. The doctest programs in
that variant link two extra files from `tests/native`:

- `tsan_options.cpp` sets the default options and points to `tsan.supp`.
- `tsan_longjmp.cpp` bypasses the `siglongjmp` interceptor. Without it each case that runs
  the engine aborts under glibc 2.44.

`tsan.supp` suppresses races TSAN reports on exception objects whose reference count lives in
uninstrumented libstdc++. Two of its entries match a frame in either stack, so in every tsan
case they also hide a race on an `exception_ptr` object itself.

Run the tsan variant through `pcons test`, which registers one test per doctest case. Case
time limits grow by a factor of three in the sanitizer variants. TSAN does not cover the Python
extension.

```sh
uv run pcons -B build-tsan --variant tsan -j10
uv run pcons test -B build-tsan -j 10 --no-build
```
