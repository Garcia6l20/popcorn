"""NFR-PERF-3: a plain run makes no Python call inside the native slices."""

from __future__ import annotations

import asyncio
import inspect
import sys
import types

import popcorn
from popcorn import _machine

from .conftest import Machines


def slice_lines() -> set[int]:
    """Lines of the facade that call into a native slice."""
    source = inspect.getsource(_machine).splitlines()
    return {n for n, line in enumerate(source, start=1) if "self._handle.run_slice(" in line}


def calls_inside_slices(
    m: popcorn.Machine, run: int, *, background: bool = False
) -> tuple[int, int]:
    """Python calls the facade made between slices, and calls made while a slice ran.

    Native code has no Python frame: a call it makes into Python has the frame that called the
    slice as its caller, stopped on the line of that call.
    """
    lines = slice_lines()
    facade = _machine.__file__
    between = 0
    inside = 0

    def profile(frame: types.FrameType, event: str, arg: object) -> None:
        nonlocal between, inside
        caller = frame.f_back
        if event != "call" or caller is None or caller.f_code.co_filename != facade:
            return
        if caller.f_lineno in lines:
            inside += 1
        elif caller.f_code.co_name in ("_drive", "_drive_background"):
            between += 1

    async def in_background() -> None:
        async with m.running():
            while m.busy and between < run:
                await asyncio.sleep(0)

    sys.setprofile(profile)
    try:
        asyncio.run(in_background() if background else m.run(budget=run))
    finally:
        sys.setprofile(None)
    return between, inside


def test_a_plain_run_makes_no_python_call_inside_its_slices(machines: Machines) -> None:
    m = machines(slice_instructions=50_000)
    between, inside = calls_inside_slices(m, 1_000_000)
    assert between >= 20
    assert inside == 0


def test_an_installed_hook_is_the_only_crossing(machines: Machines) -> None:
    m = machines(slice_instructions=50_000)
    seen: list[int] = []
    m.intercept("alu_mark", lambda call: seen.append(call.function) or call.proceed())
    _, inside = calls_inside_slices(m, 1_000_000)
    assert len(seen) == 1 and inside >= 1


def test_a_background_run_makes_no_python_call_inside_its_slices(machines: Machines) -> None:
    m = machines(slice_instructions=50_000)
    between, inside = calls_inside_slices(m, 20, background=True)
    assert between >= 20
    assert inside == 0
