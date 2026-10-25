"""Each sync mode gives one record per scenario: frames, logs, instruction counts and times.

A record holds what rule 1 of the sync design fixes: at a TIME pause every member's pc,
instruction count, time and log and the tap of each link, at a member stop the stopping member's
state, and every pause's reason, member and time. It leaves out the other members' positions
after a member stop and the tap there, which each mode sets by its own rule. Records are JSON, so
a reference record can be read from a file in place of a run.
"""

from __future__ import annotations

import asyncio
import importlib.util
import json
from collections.abc import Callable
from pathlib import Path
from types import ModuleType
from typing import Literal

import popcorn
import pytest
from popcorn import StopReason, System, SystemStop, TapFrame, UartLink
from popcorn.units import VirtualDuration, ms, us

from .linked import at, in_flight, links, logged, member, past_done

Mode = Literal["barrier", "clocks"]
Json = dict[str, object]
Scenario = Callable[[popcorn.PartDescription, int, VirtualDuration | None], Json]

LINKED_UART = Path(__file__).parents[3] / "examples" / "stm32u575" / "linked_uart"
SLICINGS = [
    pytest.param(1_000_000, None, id="plain"),
    pytest.param(997, us(3), id="997 q=3us"),
]


def frame(f: TapFrame) -> Json:
    return {
        "data": f.data,
        "index": f.index,
        "start": f.start.ps,
        "end": f.end.ps,
        "arrival": f.arrival.ps,
        "towards": f.towards,
        "injected": f.injected,
        "brk": f.brk,
        "lost": f.lost,
        "dropped": f.dropped,
        "cut": f.cut,
    }


def machine(system: System, name: str) -> Json:
    m = system.members[name]
    return {"pc": m.pc, "instructions": m.instructions, "time": m.time.ps, "log": logged(m)}


def pause(system: System, stop: SystemStop, ends: tuple[str, ...]) -> Json:
    """`stop` as the record keeps it, reading the taps of `ends` at a TIME pause."""
    seen: Json = {"reason": stop.reason.name, "member": stop.member, "time": stop.time.ps}
    if stop.reason is StopReason.TIME:
        seen["members"] = {name: machine(system, name) for name in system.members}
        seen["taps"] = {end: [frame(f) for f in system.tap(end).frames()] for end in ends}
    elif stop.member is not None:
        seen["stopping"] = machine(system, stop.member)
    return seen


def build(
    u575: popcorn.PartDescription,
    slice_instructions: int,
    third: VirtualDuration | None,
    a_entry: str,
    b_entry: str,
) -> System:
    a = member(u575, a_entry, "a", slice_instructions)
    b = member(u575, b_entry, "b", slice_instructions)
    return System({"a": a, "b": b}, links(third), slice_instructions=slice_instructions)


def pauses_to(system: System, untils: tuple[float, ...], ends: tuple[str, ...]) -> list[Json]:
    """Every pause of runs to each of `untils` us in turn, again after each member stop."""

    async def go() -> list[Json]:
        seen: list[Json] = []
        for micros in untils:
            while True:
                stop = await system.run(until=at(micros))
                seen.append(pause(system, stop, ends))
                if stop.reason is StopReason.TIME:
                    break
        return seen

    return asyncio.run(go())


def ping_echo(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> Json:
    """link_ping against link_echo: member stops on a's breakpoints, TIME pauses at 1 to 4 ms."""
    system = build(u575, slice_instructions, third, "link_ping", "link_echo")
    try:
        return {"pauses": pauses_to(system, (1000, 2000, 4000), ("a.USART1",))}
    finally:
        system.close()


def tx_rx(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> Json:
    """link_tx to link_rx: the tap at seven TIME pauses."""
    system = build(u575, slice_instructions, third, "link_tx", "link_rx")
    try:
        return {"pauses": pauses_to(system, (101, 257, 401, 733, 1001, 1777, 2999), ("a.USART1",))}
    finally:
        system.close()


def idle_cut(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> Json:
    """link_tx to link_rx with an idle cut from 600 to 1050 us, run to 3 ms."""
    system = build(u575, slice_instructions, third, "link_tx", "link_rx")
    try:
        system.tap("a.USART1").cut(at(600), at(1050), line="idle")
        return {"pauses": pauses_to(system, (3000,), ("a.USART1",))}
    finally:
        system.close()


def injected_running(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> Json:
    """link_tx to link_rx, b"xyz" injected towards b at 4 ms from the free run, run to 6 ms."""
    system = build(u575, slice_instructions, third, "link_tx", "link_rx")
    try:
        first = pauses_to(system, (3000,), ("b.USART1",))
        past_done(system)
        tap = system.tap("b.USART1")
        b = system.members["b"]

        async def free() -> None:
            async with system.running():
                await b.elapse(us(100))
                await in_flight(system)
                tap.inject(b"xyz", at=at(4000), towards="b.USART1")
                await b.elapse(ms(1.5))

        asyncio.run(free())
        return {"pauses": first + pauses_to(system, (6000,), ("b.USART1",))}
    finally:
        system.close()


def restored(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> Json:
    """link_ping against link_echo: a snapshot at 1 ms, a run to 3 ms, a restore, a run to 3 ms."""
    system = build(u575, slice_instructions, third, "link_ping", "link_echo")
    try:
        first = pauses_to(system, (1000,), ("a.USART1",))
        snapshot = system.snapshot()
        second = pauses_to(system, (3000,), ("a.USART1",))
        system.restore(snapshot)
        return {"pauses": first + second + pauses_to(system, (3000,), ("a.USART1",))}
    finally:
        system.close()


def _linked_uart() -> ModuleType:
    spec = importlib.util.spec_from_file_location(
        "linked_uart_conftest", LINKED_UART / "conftest.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def linked_uart(
    u575: popcorn.PartDescription, slice_instructions: int, third: VirtualDuration | None
) -> Json:
    """The linked_uart example's system, 300 ms in TIME pauses of 100 ms."""
    description: popcorn.SystemDescription = _linked_uart().SYSTEM
    if third is not None:
        description = description.replace(
            links=[
                *description.links,
                UartLink("controller.LPUART1", "responder1.LPUART1", latency=third, baud=460_800),
            ]
        )
    machines = description.machines()
    try:

        async def booted() -> list[Json]:
            for name, m in machines.items():
                await description.members[name].boot_machine(m)
            system = description.link(machines, slice_instructions=slice_instructions)
            try:
                seen: list[Json] = []
                for step in range(1, 4):
                    stop = await system.run(until=at(step * 100_000))
                    assert stop.reason is StopReason.TIME, stop
                    seen.append(
                        {
                            "time": stop.time.ps,
                            "members": {
                                name: {
                                    "pc": m.pc,
                                    "instructions": m.instructions,
                                    "time": m.time.ps,
                                }
                                for name, m in system.members.items()
                            },
                            "taps": {
                                towards: [frame(f) for f in system.tap(end).frames(towards=towards)]
                                for end in ("controller.UART4", "controller.USART3")
                                for towards in system.tap(end).ends
                            },
                        }
                    )
                return seen
            finally:
                system.close()

        return {"pauses": asyncio.run(booted())}
    finally:
        for m in machines.values():
            m.board.close()


SCENARIOS: dict[str, Scenario] = {
    "ping_echo": ping_echo,
    "tx_rx": tx_rx,
    "idle_cut": idle_cut,
    "injected_running": injected_running,
    "restored": restored,
    "linked_uart": linked_uart,
}


def record(
    scenario: Scenario,
    mode: Mode,
    u575: popcorn.PartDescription,
    slice_instructions: int,
    third: VirtualDuration | None,
    monkeypatch: pytest.MonkeyPatch,
) -> str:
    """The canonical JSON of `scenario` run in `mode`."""
    with monkeypatch.context() as patched:
        patched.setattr(System, "_sync", mode)
        return json.dumps(scenario(u575, slice_instructions, third), sort_keys=True)


@pytest.fixture
def reference(
    u575: popcorn.PartDescription, monkeypatch: pytest.MonkeyPatch
) -> Callable[[str, int, VirtualDuration | None], str]:
    """The reference record of a scenario: its run in barrier mode."""

    def of(name: str, slice_instructions: int, third: VirtualDuration | None) -> str:
        return record(SCENARIOS[name], "barrier", u575, slice_instructions, third, monkeypatch)

    return of


@pytest.mark.parametrize(("slice_instructions", "third"), SLICINGS)
@pytest.mark.parametrize("name", list(SCENARIOS))
def test_a_scenario_gives_its_reference_record_in_clock_mode(
    u575: popcorn.PartDescription,
    monkeypatch: pytest.MonkeyPatch,
    reference: Callable[[str, int, VirtualDuration | None], str],
    name: str,
    slice_instructions: int,
    third: VirtualDuration | None,
) -> None:
    seen = record(SCENARIOS[name], "clocks", u575, slice_instructions, third, monkeypatch)
    expected = reference(name, slice_instructions, third)
    assert json.loads(seen) == json.loads(expected)


def test_the_records_hold_frames_member_stops_and_time_pauses(
    u575: popcorn.PartDescription, monkeypatch: pytest.MonkeyPatch
) -> None:
    pauses = json.loads(record(ping_echo, "clocks", u575, 1_000_000, None, monkeypatch))["pauses"]
    reasons = [entry["reason"] for entry in pauses]
    assert "USER" in reasons and reasons[-1] == "TIME"
    taps = [entry["taps"]["a.USART1"] for entry in pauses if entry["reason"] == "TIME"]
    assert sum(len(frames) for frames in taps) >= 32
