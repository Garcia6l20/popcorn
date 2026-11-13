"""Removing an intercept, a return hook or a watch from a handler of its own machine."""

from __future__ import annotations

import asyncio
import contextlib
import threading
import weakref

import popcorn
import pytest

from .conftest import HOOKS_ELF, collected

SLICES = [1, 50, 1_000_000]


def machine(m33: popcorn.PartDescription, slice_instructions: int) -> popcorn.Machine:
    m = popcorn.Machine(
        m33, image=popcorn.Image.elf(HOOKS_ELF), slice_instructions=slice_instructions
    )
    m.reset()
    assert asyncio.run(m.run(budget=100_000)).reason is popcorn.StopReason.USER
    return m


def call(m: popcorn.Machine, fn: str) -> popcorn.CallResult:
    return asyncio.run(m.call(fn, budget=1_000_000))


@pytest.mark.parametrize("slice_instructions", SLICES)
def test_an_intercept_removed_from_its_own_handler_is_not_called_again(
    m33: popcorn.PartDescription, slice_instructions: int
) -> None:
    m = machine(m33, slice_instructions)
    seen: list[int] = []
    inside: list[bool] = []
    holder: list[popcorn.Intercept] = []

    def handler(c: popcorn.Call) -> popcorn.Return:
        seen.append(c.occurrence)
        holder[0].remove()
        inside.append(holder[0].active)
        return c.proceed()

    holder.append(m.intercept("writer", handler))
    call(m, "write_three")
    assert seen == [0]
    assert inside == [False]
    assert not holder[0].active
    call(m, "write_three")
    assert seen == [0]
    assert m.var("shared").value == 3


@pytest.mark.parametrize("slice_instructions", SLICES)
def test_a_return_hook_removed_from_its_handler_does_not_run_on_the_next_return(
    m33: popcorn.PartDescription, slice_instructions: int
) -> None:
    m = machine(m33, slice_instructions)
    seen: list[int] = []
    inside: list[bool] = []
    holder: list[popcorn.Intercept] = []

    def handler(r: popcorn.Returned) -> None:
        seen.append(r.occurrence)
        holder[0].remove()
        inside.append(holder[0].active)

    holder.append(m.on_return("writer", handler))
    call(m, "write_three")
    assert seen == [0]
    assert inside == [False]
    assert not holder[0].active
    call(m, "write_three")
    assert seen == [0]


@pytest.mark.parametrize("slice_instructions", SLICES)
def test_a_watch_removed_from_its_handler_is_not_called_again(
    m33: popcorn.PartDescription, slice_instructions: int
) -> None:
    m = machine(m33, slice_instructions)
    seen: list[int] = []
    inside: list[bool] = []
    holder: list[popcorn.Watch] = []

    def handler(hit: popcorn.WatchHit) -> None:
        seen.append(hit.value)
        holder[0].remove()
        inside.append(holder[0].active)

    holder.append(m.watch("shared", handler=handler))
    call(m, "write_three")
    assert len(seen) == 1
    assert inside == [False]
    assert not holder[0].active
    call(m, "write_three")
    assert len(seen) == 1


@pytest.mark.parametrize("slice_instructions", SLICES)
def test_a_handler_removing_another_hook_of_its_machine_stops_that_hook(
    m33: popcorn.PartDescription, slice_instructions: int
) -> None:
    m = machine(m33, slice_instructions)
    other_seen: list[int] = []
    holder: list[popcorn.Intercept] = []
    other = m.on_return("writer", lambda r: other_seen.append(r.occurrence))

    def handler(c: popcorn.Call) -> popcorn.Return:
        if c.occurrence == 0:
            other.remove()
            holder[0].remove()
        return c.proceed()

    holder.append(m.intercept("writer", handler))
    call(m, "write_three")
    assert other_seen == []
    assert not other.active


def test_removing_from_a_handler_twice_does_nothing(m33: popcorn.PartDescription) -> None:
    m = machine(m33, 1_000_000)
    seen: list[int] = []
    holder: list[popcorn.Intercept] = []

    def handler(c: popcorn.Call) -> popcorn.Return:
        seen.append(c.occurrence)
        holder[0].remove()
        holder[0].remove()
        return c.proceed()

    holder.append(m.intercept("writer", handler))
    call(m, "write_three")
    holder[0].remove()
    assert seen == [0]


def test_removing_from_another_thread_while_the_machine_runs_raises(
    m33: popcorn.PartDescription,
) -> None:
    m = machine(m33, 1_000_000)
    entered = threading.Event()
    release = threading.Event()

    def handler(c: popcorn.Call) -> popcorn.Return:
        entered.set()
        assert release.wait(10)
        return c.proceed()

    blocking = m.intercept("writer", handler)
    victim = m.on_return("write_three", lambda r: None)
    outcome: list[popcorn.CallResult] = []
    runner = threading.Thread(target=lambda: outcome.append(call(m, "write_three")))
    runner.start()
    try:
        assert entered.wait(10)
        with pytest.raises(popcorn.MachineRunning):
            victim.remove()
        assert victim.active
    finally:
        release.set()
        runner.join(10)
    assert len(outcome) == 1
    victim.remove()
    blocking.remove()
    assert not victim.active
    assert not blocking.active


@pytest.mark.parametrize("kind", ["intercept", "return", "watch"])
def test_a_handler_removed_from_itself_is_freed_when_the_slice_ends(
    m33: popcorn.PartDescription, kind: str
) -> None:
    m = machine(m33, 1_000_000)
    holder: list[popcorn.Intercept | popcorn.Watch] = []

    def intercept(c: popcorn.Call) -> popcorn.Return:
        holder[0].remove()
        return c.proceed()

    def returned(_: popcorn.Returned) -> None:
        holder[0].remove()

    def watch(_: popcorn.WatchHit) -> None:
        holder[0].remove()

    if kind == "intercept":
        holder.append(m.intercept("writer", intercept))
    elif kind == "return":
        holder.append(m.on_return("writer", returned))
    else:
        holder.append(m.watch("shared", handler=watch))
    ref = weakref.ref({"intercept": intercept, "return": returned, "watch": watch}[kind])
    del intercept, returned, watch
    call(m, "write_three")
    holder.clear()
    assert collected(ref)


def test_a_handler_that_removes_itself_and_raises_still_ends_the_hook(
    m33: popcorn.PartDescription,
) -> None:
    m = machine(m33, 1_000_000)
    holder: list[popcorn.Intercept] = []

    def handler(c: popcorn.Call) -> popcorn.Return:
        holder[0].remove()
        raise KeyError("boom")

    holder.append(m.intercept("writer", handler))
    with pytest.raises(KeyError):
        call(m, "write_three")
    assert not holder[0].active
    call(m, "write_three")


def test_install_and_remove_from_the_handler_leaves_nothing_behind(
    m33: popcorn.PartDescription,
) -> None:
    m = machine(m33, 1_000_000)
    table = m._callback_table  # pyright: ignore[reportPrivateUsage]
    before = len(table)
    for _ in range(1000):
        holder: list[popcorn.Intercept] = []

        def handler(c: popcorn.Call, holder: list[popcorn.Intercept] = holder) -> popcorn.Return:
            holder[0].remove()
            return c.proceed()

        holder.append(m.intercept("writer", handler))
        call(m, "write_three")
    assert len(table) == before
    assert m.remove_hooks() == 0


@pytest.mark.parametrize("removes", [False, True])
def test_a_removed_hook_takes_the_gil_for_the_calls_it_ran_and_the_release_of_its_two_relays(
    m33: popcorn.PartDescription, removes: bool
) -> None:
    m = machine(m33, 1_000_000)
    holder: list[popcorn.Intercept] = []

    def handler(c: popcorn.Call) -> popcorn.Return:
        if removes:
            holder[0].remove()
        return c.proceed()

    holder.append(m.intercept("writer", handler))
    before = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
    call(m, "write_three")
    entries = m._handle.gil_entries() - before  # pyright: ignore[reportPrivateUsage]
    calls = 3 + 3
    assert entries == (1 + 2 if removes else calls)


def test_a_handler_removed_from_itself_is_freed_with_its_machine(
    m33: popcorn.PartDescription,
) -> None:
    m = machine(m33, 1_000_000)
    holder: list[popcorn.Intercept] = []

    def handler(c: popcorn.Call) -> popcorn.Return:
        holder[0].remove()
        return c.stop(popcorn.StopReason.USER)

    holder.append(m.intercept("writer", handler))
    ref = weakref.ref(handler)
    machine_ref = weakref.ref(m)
    del handler
    with contextlib.suppress(Exception):
        call(m, "write_three")
    assert not holder[0].active
    holder.clear()
    del m
    assert collected(ref)
    assert collected(machine_ref)


@pytest.mark.parametrize("slice_instructions", SLICES)
def test_a_hook_whose_last_reference_a_handler_drops_runs_no_more_in_that_slice(
    m33: popcorn.PartDescription, slice_instructions: int
) -> None:
    m = machine(m33, slice_instructions)
    seen: list[int] = []
    holder: list[popcorn.Intercept] = []

    def handler(c: popcorn.Call) -> popcorn.Return:
        seen.append(c.occurrence)
        holder.clear()
        return c.proceed()

    holder.append(m.intercept("writer", handler))
    m._installed.clear()  # pyright: ignore[reportPrivateUsage]
    call(m, "write_three")
    assert seen == [0]
    call(m, "write_three")
    assert seen == [0]
