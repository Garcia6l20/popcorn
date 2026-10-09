"""NFR-PERF-3: a plain run makes no Python call inside the native slices."""

from __future__ import annotations

import asyncio
import gc
import threading
import types

import popcorn
from popcorn import _host
from popcorn.units import VirtualDuration

from .conftest import Machines

BUDGET = 1_000_000


def from_bridge(frame: types.FrameType | None) -> bool:
    """Whether the call comes from the bridge that completes an awaited request on the loop:
    it runs on the machine's thread after the slices, not inside one."""
    while frame is not None:
        if frame.f_code.co_filename == _host.__file__:
            return True
        frame = frame.f_back
    return False


def calls_inside_slices(
    m: popcorn.Machine, *, free_for: VirtualDuration | None = None
) -> list[str]:
    """Python calls made on the machine's thread while it ran `BUDGET` instructions, or a free
    run of `free_for`.

    A slice runs on the machine's thread, so a call made while it runs is a call on another
    thread than the test's. The profiler covers every thread, the machine's included, which
    exists once the machine ran. The cycle collector is off meanwhile: it runs finalizers of
    earlier tests' garbage on whichever thread allocates, the machine's included.
    """
    asyncio.run(m.step(1))
    test_thread = threading.get_ident()
    inside: list[str] = []

    def profile(frame: types.FrameType, event: str, arg: object) -> None:
        if event == "call" and threading.get_ident() != test_thread and not from_bridge(frame):
            inside.append(f"{frame.f_code.co_filename}:{frame.f_code.co_name}")

    async def free_run(duration: VirtualDuration) -> None:
        m.start()
        try:
            waiter = m._handle.wait(None, duration.ps, None)  # pyright: ignore[reportPrivateUsage]
            await _host.awaited(waiter, lambda raw: raw)
        finally:
            m.halt()

    gc.collect()
    gc.disable()
    threading.setprofile_all_threads(profile)
    try:
        if free_for is None:
            stop = asyncio.run(m.run(budget=BUDGET))
            assert stop.reason is popcorn.StopReason.BUDGET
        else:
            asyncio.run(free_run(free_for))
    finally:
        threading.setprofile_all_threads(None)
        gc.enable()
    return inside


def test_a_plain_run_makes_no_python_call_inside_its_slices(machines: Machines) -> None:
    m = machines(slice_instructions=50_000)
    assert calls_inside_slices(m) == []


def test_an_installed_hook_is_the_only_crossing(machines: Machines) -> None:
    m = machines(slice_instructions=50_000)
    seen: list[int] = []
    m.intercept("alu_mark", lambda call: seen.append(call.function) or call.proceed())
    asyncio.run(m.step(1))
    before = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    calls_inside_slices(m)
    assert len(seen) == 1
    entries = m._handle.gil_entries() - before  # pyright: ignore[reportPrivateUsage]
    assert entries == 2 * len(seen), "the intercept and the return hook logging its call"


def test_a_background_run_makes_no_python_call_inside_its_slices(machines: Machines) -> None:
    twin = machines(slice_instructions=50_000)
    took = asyncio.run(twin.run(budget=BUDGET)).time
    m = machines(slice_instructions=50_000)
    before = m.instructions
    assert calls_inside_slices(m, free_for=VirtualDuration(took.ps)) == []
    assert m.instructions - before >= BUDGET // 2
