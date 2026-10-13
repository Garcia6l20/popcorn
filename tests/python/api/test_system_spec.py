"""`popcorn.SystemDescription`: members and links as data, checked at construction, built into a
`System`."""

from __future__ import annotations

import asyncio
import dataclasses
from collections.abc import Callable
from pathlib import Path

import popcorn
import pytest
from popcorn import ConfigError, MachineDescription, StopReason, SystemDescription, UartLink
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .linked import PING_BYTES, USART_LL, logged, received

L = us(20)

PROJECT = MachineDescription(
    part="STM32U575QGIx",
    images=[popcorn.Elf(USART_LL)],
    boot=popcorn.Boot(until="main", budget=1_000_000),
)
ABSENT = MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(Path("/nonexistent/fw.elf"))])


def link(a: object = "a.USART1", b: object = "b.USART1", **fields: object) -> UartLink:
    values: dict[str, object] = {"latency": L, "baud": 460_800, **fields}
    return UartLink(a, b, **values)  # pyright: ignore[reportArgumentType]


def spec(**changes: object) -> SystemDescription:
    values: dict[str, object] = {
        "members": {"a": PROJECT, "b": PROJECT},
        "links": [link()],
        **changes,
    }
    return SystemDescription(**values)  # pyright: ignore[reportArgumentType]


def test_a_valid_system_is_a_frozen_hashable_value() -> None:
    one = spec(links=[link(), link("a.USART2", "b.USART2", latency=us(7))], offsets={"b": us(3)})
    assert one.quantum == us(7)
    assert list(one.members) == ["a", "b"]
    assert isinstance(one.links, tuple)
    assert one == spec(
        links=[link(), link("a.USART2", "b.USART2", latency=us(7))], offsets={"b": us(3)}
    )
    assert hash(one) == hash(
        spec(links=[link(), link("a.USART2", "b.USART2", latency=us(7))], offsets={"b": us(3)})
    )
    assert one != spec(
        links=[link(), link("a.USART2", "b.USART2", latency=us(8))], offsets={"b": us(3)}
    )
    with pytest.raises(dataclasses.FrozenInstanceError):
        one.goal_cap = ms(1)  # pyright: ignore[reportAttributeAccessIssue]
    with pytest.raises(TypeError):
        one.members["c"] = PROJECT  # pyright: ignore[reportIndexIssue]
    assert one.replace(goal_cap=ms(1)).goal_cap == ms(1)
    assert one.goal_cap is None and one.on_member_reset == "reset"


def test_a_system_of_absent_images_is_declared_without_reading_them() -> None:
    declared = SystemDescription(members={"a": ABSENT, "b": ABSENT}, links=[link()])
    assert declared.members["a"] is ABSENT


def test_a_link_between_two_uarts_of_one_member_is_accepted() -> None:
    lone = SystemDescription(members={"a": PROJECT}, links=[link("a.USART1", "a.USART3")])
    assert lone.quantum == L


CHECKS: list[tuple[str, Callable[[], object]]] = [
    ("members", lambda: spec(members={})),
    ("members.a.b", lambda: spec(members={"a.b": PROJECT})),
    ("members.a b", lambda: spec(members={"a b": PROJECT})),
    ("members.b", lambda: spec(members={"a": PROJECT, "b": "project"})),
    ("links", lambda: spec(links=[])),
    ("links[0]", lambda: spec(links=[("a.USART1", "b.USART1")])),
    ("links[0].latency", lambda: spec(links=[link(latency=us(0))])),
    ("links[0].latency", lambda: spec(links=[link(latency=VirtualDuration(-1))])),
    ("links[0].latency", lambda: spec(links=[link(latency=20)])),
    ("links[0].latency", lambda: spec(links=[link(latency=None)])),
    ("links[0].baud", lambda: spec(links=[link(baud=0)])),
    ("links[0].baud", lambda: spec(links=[link(baud="fast")])),
    ("links[0].baud", lambda: spec(links=[link(baud=True)])),
    ("links[0].data_bits", lambda: spec(links=[link(data_bits=10)])),
    ("links[0].data_bits", lambda: spec(links=[link(data_bits=9, parity="even")])),
    ("links[0].stop_bits", lambda: spec(links=[link(stop_bits=3)])),
    ("links[0].stop_bits", lambda: spec(links=[link(stop_bits=True)])),
    ("links[0].parity", lambda: spec(links=[link(parity="mark")])),
    ("links[0].a", lambda: spec(links=[link(a="USART1")])),
    ("links[0].a", lambda: spec(links=[link(a="a.")])),
    ("links[0].a", lambda: spec(links=[link(a=1)])),
    ("links[0].b", lambda: spec(links=[link(b="c.USART1")])),
    ("links[0].b", lambda: spec(links=[link(b="a.USART1")])),
    ("links[1].a", lambda: spec(links=[link(), link("a.USART1", "b.USART2")])),
    ("offsets.c", lambda: spec(offsets={"c": us(1)})),
    ("offsets.a", lambda: spec(offsets={"a": VirtualDuration(-1)})),
    ("offsets.a", lambda: spec(offsets={"a": 5})),
    ("goal_cap", lambda: spec(goal_cap=us(0))),
    ("goal_cap", lambda: spec(goal_cap=1000)),
    ("on_member_reset", lambda: spec(on_member_reset="halt")),
    ("links", lambda: spec().replace(links=[])),
]


@pytest.mark.parametrize(
    ("key", "make"), CHECKS, ids=[f"{n}-{k}" for n, (k, _) in enumerate(CHECKS)]
)
def test_each_check_raises_naming_its_key(key: str, make: Callable[[], object]) -> None:
    with pytest.raises(ConfigError) as caught:
        make()
    assert caught.value.key == key


def test_a_link_without_latency_cannot_be_declared() -> None:
    with pytest.raises(TypeError, match="latency"):
        UartLink("a.USART1", "b.USART1", baud=460_800)  # pyright: ignore[reportCallIssue]


def test_link_needs_one_machine_per_member(u575: popcorn.PartDescription) -> None:
    del u575
    declared = spec()
    a = PROJECT.machine(name="a")
    with pytest.raises(ConfigError) as caught:
        declared.link({"a": a})
    assert caught.value.key == "members.b"
    with pytest.raises(ConfigError) as caught:
        declared.link({"a": a, "b": PROJECT.machine(name="b"), "c": PROJECT.machine(name="c")})
    assert caught.value.key == "members.c"
    with pytest.raises(ConfigError) as caught:
        declared.link({"a": a, "b": PROJECT.machine(name="x")})
    assert caught.value.key == "members.b"


def test_machines_builds_a_named_machine_per_member(u575: popcorn.PartDescription) -> None:
    del u575
    machines = spec().machines()
    assert list(machines) == ["a", "b"]
    assert [m.name for m in machines.values()] == ["a", "b"]
    assert machines["a"] is not machines["b"]


def test_boot_links_members_booted_from_reset(u575: popcorn.PartDescription) -> None:
    del u575
    declared = spec(offsets={"b": us(5)}, goal_cap=ms(50), on_member_reset="stop")

    async def run() -> None:
        with await declared.boot() as system:
            a, b = system.members["a"], system.members["b"]
            assert a.pc == a.sym("main") and b.pc == b.sym("main")
            assert system.quantum == L
            assert system.time_of("b").ps - system.time_of("a").ps == us(5).ps
            a.reg.pc = a.sym("link_ping")
            b.reg.pc = b.sym("link_echo")
            stop = await system.run(for_=ms(3))
            assert (stop.reason, stop.member) == (StopReason.USER, "a")
            assert received(a) == [0x40 + index for index in range(PING_BYTES)]
            assert logged(b) == []

    asyncio.run(run())


def test_two_members_of_one_project_share_a_boot_and_keep_their_own_state(
    u575: popcorn.PartDescription,
) -> None:
    del u575
    declared = SystemDescription(
        members={"a": PROJECT, "b1": PROJECT, "b2": PROJECT},
        links=[link("a.USART1", "b1.USART1"), link("a.USART2", "b2.USART1")],
    )
    first = PROJECT.machine(name="b1")
    asyncio.run(PROJECT.boot_machine(first))
    saved = first.snapshot().to_bytes()
    second = PROJECT.machine(name="b2")
    second.restore(popcorn.Snapshot.from_bytes(saved))
    a = PROJECT.machine(name="a")
    a.restore(popcorn.Snapshot.from_bytes(saved))
    with declared.link({"a": a, "b1": first, "b2": second}) as system:
        assert first.pc == second.pc == first.sym("main")
        a.reg.pc = a.sym("link_ping")
        first.reg.pc = first.sym("link_echo")
        second.reg.pc = second.sym("link_rx")
        stop = asyncio.run(system.run(for_=ms(3)))
        assert (stop.reason, stop.member) == (StopReason.USER, "a")
        done = asyncio.run(system.run(until=VirtualTime(ms(3).ps)))
        assert done.reason is StopReason.TIME
        assert received(a) == [0x40 + index for index in range(PING_BYTES)]
        assert received(first) == []
        assert received(second) == []
        assert first.mem.u32[first.sym("log_len")] != a.mem.u32[a.sym("log_len")]
        snapshot = system.snapshot()
        assert snapshot.members["b1"].to_bytes() != snapshot.members["b2"].to_bytes()


def test_a_system_of_one_member_linked_to_itself_runs(u575: popcorn.PartDescription) -> None:
    del u575
    declared = SystemDescription(members={"a": PROJECT}, links=[link("a.USART1", "a.USART3")])

    async def run() -> None:
        with await declared.boot() as system:
            a = system.members["a"]
            a.reg.pc = a.sym("link_self")
            stop = await system.run(for_=ms(3))
            assert (stop.reason, stop.member) == (StopReason.USER, "a")
            assert received(a) == [0x40 + index for index in range(PING_BYTES)]
            frames = system.tap("a.USART3").frames()
            assert all(frame.arrival.ps == frame.end.ps + L.ps for frame in frames)

    asyncio.run(run())
