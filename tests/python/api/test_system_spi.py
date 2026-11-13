"""`popcorn.SpiBus` and `SpiTap`: an SPI master and slaves on other members of a linked system,
built from the spi_ll link firmware."""

from __future__ import annotations

import asyncio
import gc
import os
import weakref
from collections.abc import Callable, Iterator, Mapping, Sequence
from pathlib import Path
from typing import Any, cast

import popcorn
import pytest
from popcorn import (
    ConfigError,
    SpiBus,
    SpiTap,
    SpiTapFrame,
    System,
    SystemDescription,
    UartLink,
    UartTap,
)
from popcorn.board import SpiController, SpiTarget
from popcorn.stm32.description._shipped import build_text
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .conftest import collected
from .linked import VARIANT_VALUES, VARIANTS, in_flight, to
from .test_system_state import encoded

SPI_LL = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "spi_ll.elf"
PIN_DATA_VARIABLE = "POPCORN_STM32_PIN_DATA"
AREA = 0x20080000
MAGIC = 0x5B1A11
MASTER = 1
SLAVE = 2
CS_LOW = 1
CS_HIGH = 2
SEND = 3
WAIT_US = 4
HARD_NSS = 6
RCC = 7
KICK = 10
FORMAT = 11
ABORT = 12
L = us(20)
FRAME = us(4)
END = VirtualTime(ms(1).ps)

Build = Callable[..., System]


def op(code: int, arg: int = 0) -> int:
    return code << 24 | arg


def cs_low(pin: int) -> int:
    return op(CS_LOW, pin)


def cs_high(pin: int) -> int:
    return op(CS_HIGH, pin)


def send(count: int, first: int) -> int:
    return op(SEND, count << 8 | first)


def wait_us(micros: int) -> int:
    return op(WAIT_US, micros)


def spi_member(
    part: popcorn.PartDescription,
    entry: int,
    name: str,
    script: Sequence[int] = (),
    *,
    prefill: int = 0,
    slice_instructions: int = 1_000_000,
    devices: Mapping[str, Sequence[object]] | None = None,
) -> popcorn.Machine:
    """A spi_ll machine named `name` running the link firmware as master or slave, `script` its
    steps, the slave's first reply byte `prefill` when not 0."""
    m = popcorn.Machine(
        part,
        image=popcorn.Image.elf(SPI_LL),
        name=name,
        slice_instructions=slice_instructions,
        devices=cast(Any, devices),
    )
    m.reset()
    stop = asyncio.run(m.run(until="main", budget=1_000_000))
    assert stop.reason is popcorn.StopReason.ADDRESS, stop
    words = m.mem.u32
    for offset, value in ((0, MAGIC), (4, entry), (8, 0), (12, 0), (16, 0), (20, prefill)):
        words[AREA + offset] = value
    for index in range(64):
        words[AREA + 24 + 4 * index] = script[index] if index < len(script) else 0
    m.reg.pc = m.sym("link_run")
    return m


def logged(m: popcorn.Machine) -> list[int]:
    """The firmware's link log words."""
    count = m.mem.u32[AREA + 12]
    return [m.mem.u32[AREA + 280 + 4 * index] for index in range(min(count, 256))]


def master_script(frames: int = 3, wait: int = 50, pin: int = 4) -> list[int]:
    return [cs_high(pin), wait_us(wait), cs_low(pin), send(frames, 0x10), cs_high(pin)]


def bus_of(**changes: object) -> SpiBus:
    values: dict[str, object] = {
        "master": "a.SPI1",
        "slaves": {"a.PA4": "b.SPI1"},
        "latency": L,
        **changes,
    }
    return SpiBus(**values)  # pyright: ignore[reportArgumentType]


@pytest.fixture
def pair(u575: popcorn.PartDescription) -> Iterator[Build]:
    """Builds `a`, the master, clocking `b` on PA4, over the bus `bus`."""
    made: list[System] = []

    def build(
        slice_instructions: int = 1_000_000,
        third: VirtualDuration | None = None,
        *,
        script: Sequence[int] | None = None,
        bus: SpiBus | None = None,
    ) -> System:
        a = spi_member(
            u575,
            MASTER,
            "a",
            master_script() if script is None else script,
            slice_instructions=slice_instructions,
        )
        b = spi_member(u575, SLAVE, "b", slice_instructions=slice_instructions)
        links: list[SpiBus | UartLink] = [bus or bus_of()]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        system = System({"a": a, "b": b}, links, slice_instructions=slice_instructions)
        made.append(system)
        return system

    yield build
    for one in made:
        one.close()


def observed(system: System) -> tuple[list[int], list[int]]:
    return logged(system.members["a"]), logged(system.members["b"])


def test_a_bus_gives_the_master_the_slaves_replies_and_the_tap_every_frame_at_any_quantum(
    pair: Build,
) -> None:
    results: list[tuple[list[SpiTapFrame], tuple[list[int], list[int]]]] = []
    for slice_instructions, third in VARIANT_VALUES:
        system = pair(slice_instructions, third)
        tap = system.tap("a.SPI1", SpiTap)
        assert isinstance(tap, SpiTap)
        asyncio.run(to(system, END))
        results.append((tap.frames(), observed(system)))
    assert all(result == results[0] for result in results[1:])
    seen, (master, slave) = results[0]
    assert [(f.mosi, f.miso) for f in seen] == [(0x10, 0xA0), (0x11, 0x20), (0x12, 0x21)]
    assert all(f.slave == "b.SPI1" and f.cs == "PA4" and f.bits == 8 for f in seen)
    assert not any(f.dropped for f in seen)
    assert all(f.reply.ps == f.at.ps + L.ps + FRAME.ps for f in seen)
    assert [f.unit for f in seen] == sorted({f.unit for f in seen})
    assert master[1:] == [0xA0, 0x20, 0x21]
    assert slave[1:] == [0x5110, 0x5111, 0x5112]


def test_the_tap_of_either_end_is_one_object_and_shows_the_bus(pair: Build) -> None:
    system = pair()
    tap = system.tap("a.SPI1", SpiTap)
    assert system.tap("b.SPI1", SpiTap) is tap
    assert system.tap("a.SPI1", SpiTap) is tap
    assert tap.ends == ("a.SPI1", "b.SPI1")
    assert repr(tap) == "SpiTap(a.SPI1 -> b.SPI1)"
    assert (tap.available, tap.dropped) == (0, 0)


def free_run(
    system: System, running: bool
) -> tuple[list[SpiTapFrame], tuple[list[int], ...], tuple[object, ...]]:
    """The tap and the logs at 1 ms of system time, reached by a free run to 0.5 ms when
    `running` and by TIME stops otherwise."""
    tap = system.tap("a.SPI1", SpiTap)
    b = system.members["b"]

    async def free() -> None:
        async with system.running():
            await b.elapse(us(100))
            await in_flight(system)
            await b.elapse(us(500))

    if running:
        asyncio.run(free())
    asyncio.run(to(system, END))
    return tap.frames(), observed(system), encoded(system)


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_a_free_run_gives_the_frames_and_logs_of_the_parked_run(
    pair: Build, slice_instructions: int, third: VirtualDuration | None
) -> None:
    parked = free_run(pair(slice_instructions, third), False)
    running = free_run(pair(slice_instructions, third), True)
    assert running == parked
    assert len(parked[0]) == 3


def test_two_gpio_slaves_and_an_ss_output_slave_each_see_their_own_frames(
    u575: popcorn.PartDescription,
) -> None:
    script = [
        cs_high(4),
        cs_high(5),
        wait_us(50),
        cs_low(4),
        send(2, 0x10),
        cs_high(4),
        cs_low(5),
        send(2, 0x30),
        cs_high(5),
    ]
    a = spi_member(u575, MASTER, "a", script)
    b = spi_member(u575, SLAVE, "b")
    c = spi_member(u575, SLAVE, "c", prefill=0x3C)
    bus = bus_of(slaves={"a.PA4": "b.SPI1", "a.PA5": "c.SPI1"})
    system = System({"a": a, "b": b, "c": c}, [bus])
    try:
        tap = system.tap("c.SPI1", SpiTap)
        asyncio.run(to(system, END))
        both = tap.frames()
        assert [(f.slave, f.cs, f.mosi, f.miso) for f in both] == [
            ("b.SPI1", "PA4", 0x10, 0xA0),
            ("b.SPI1", "PA4", 0x11, 0x20),
            ("c.SPI1", "PA5", 0x30, 0x3C),
            ("c.SPI1", "PA5", 0x31, 0x40),
        ]
        assert [f.at.ps for f in both] == sorted(f.at.ps for f in both)
        assert tap.available == 0
    finally:
        system.close()


def test_frames_of_one_slave_leave_the_others_in_the_tap(u575: popcorn.PartDescription) -> None:
    script = [
        cs_high(4),
        cs_high(5),
        wait_us(50),
        cs_low(4),
        send(1, 0x10),
        cs_high(4),
        cs_low(5),
        send(1, 0x30),
        cs_high(5),
    ]
    a = spi_member(u575, MASTER, "a", script)
    b = spi_member(u575, SLAVE, "b")
    c = spi_member(u575, SLAVE, "c")
    system = System(
        {"a": a, "b": b, "c": c}, [bus_of(slaves={"a.PA4": "b.SPI1", "a.PA5": "c.SPI1"})]
    )
    try:
        tap = system.tap("a.SPI1", SpiTap)
        asyncio.run(to(system, END))
        taken = tap.frames(slave="c.SPI1")
        assert [(f.slave, f.mosi) for f in taken] == [("c.SPI1", 0x30)]
        assert tap.available == 1
        with pytest.raises(ValueError, match="no slave of"):
            tap.frames(slave="a.SPI1")
        with pytest.raises(TypeError, match="slave must be a str"):
            tap.frames(slave=cast(Any, 3))
        assert [f.slave for f in tap.frames()] == ["b.SPI1"]
    finally:
        system.close()


@pytest.mark.parametrize("first", [4, 5])
@pytest.mark.parametrize("lead", range(40, 80, 3))
def test_frames_of_two_slaves_come_out_in_the_order_of_the_wire_whichever_is_first(
    u575: popcorn.PartDescription, lead: int, first: int
) -> None:
    second = 9 - first
    script = [
        cs_high(4),
        cs_high(5),
        wait_us(lead),
        cs_low(first),
        send(1, 0x10),
        cs_high(first),
        cs_low(second),
        send(1, 0x30),
        cs_high(second),
    ]
    a = spi_member(u575, MASTER, "a", script)
    b = spi_member(u575, SLAVE, "b")
    c = spi_member(u575, SLAVE, "c")
    system = System(
        {"a": a, "b": b, "c": c},
        [bus_of(slaves={"a.PA4": "b.SPI1", "a.PA5": "c.SPI1"}, latency=us(200))],
    )
    try:
        tap = system.tap("a.SPI1", SpiTap)
        asyncio.run(to(system, VirtualTime(ms(3).ps)))
        seen = tap.frames()
        slaves = {4: "b.SPI1", 5: "c.SPI1"}
        assert [(f.slave, f.mosi) for f in seen] == [
            (slaves[first], 0x10),
            (slaves[second], 0x30),
        ]
        assert seen[0].at.ps < seen[1].at.ps
    finally:
        system.close()


LONG = us(200)
SPLIT = VirtualTime(us(500).ps)
LONG_VARIANTS: list[tuple[int, VirtualDuration | None]] = [
    (1_000_000, None),
    (997, None),
    (1_000_000, us(100)),
    (997, us(100)),
]


def given_up(first: int, give_up: int) -> list[int]:
    """F0 to the slave on PA`first`, the master giving it up 2 us after its end by `give_up`, then
    F1 to the other slave."""
    second = 9 - first
    return [
        cs_high(4),
        cs_high(5),
        cs_low(first),
        op(KICK, 0x10),
        wait_us(6),
        give_up,
        cs_high(first),
        cs_low(second),
        op(KICK, 0x30),
    ]


@pytest.mark.parametrize("give_up", [op(ABORT), op(RCC)], ids=["spe", "rcc"])
@pytest.mark.parametrize("first", [4, 5])
def test_frames_of_two_slaves_the_master_gave_up_come_out_in_at_order_at_any_quantum(
    u575: popcorn.PartDescription, first: int, give_up: int
) -> None:
    runs: list[list[tuple[str, int, int]]] = []
    for slice_instructions, third in LONG_VARIANTS:
        a = spi_member(
            u575, MASTER, "a", given_up(first, give_up), slice_instructions=slice_instructions
        )
        b = spi_member(u575, SLAVE, "b", slice_instructions=slice_instructions)
        c = spi_member(u575, SLAVE, "c", slice_instructions=slice_instructions)
        links: list[SpiBus | UartLink] = [
            bus_of(slaves={"a.PA4": "b.SPI1", "a.PA5": "c.SPI1"}, latency=LONG)
        ]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        system = System({"a": a, "b": b, "c": c}, links, slice_instructions=slice_instructions)
        try:
            tap = system.tap("a.SPI1", SpiTap)
            asyncio.run(to(system, VirtualTime(ms(2).ps)))
            seen = tap.frames()
        finally:
            system.close()
        assert len(seen) == 2
        handed = [f.reply.ps + LONG.ps for f in seen]
        assert min(handed) < SPLIT.ps < max(handed)
        assert [f.at.ps for f in seen] == sorted(f.at.ps for f in seen)
        runs.append([(f.slave, f.mosi, f.at.ps) for f in seen])
    assert all(run == runs[0] for run in runs[1:])
    slaves = {4: "b.SPI1", 5: "c.SPI1"}
    assert [(slave, mosi) for slave, mosi, _ in runs[0]] == [
        (slaves[first], 0x10),
        (slaves[9 - first], 0x30),
    ]


def test_a_slow_frame_handed_after_a_later_fast_one_is_read_first_across_paused_reads(
    u575: popcorn.PartDescription,
) -> None:
    script = [
        cs_high(4),
        cs_high(5),
        op(FORMAT, 8 << 8 | 7),
        cs_low(5),
        op(KICK, 0x10),
        wait_us(518),
        op(RCC),
        cs_low(4),
        op(KICK, 0x30),
    ]
    a = spi_member(u575, MASTER, "a", script)
    b = spi_member(u575, SLAVE, "b")
    c = spi_member(u575, SLAVE, "c")
    system = System(
        {"a": a, "b": b, "c": c},
        [bus_of(slaves={"a.PA4": "b.SPI1", "a.PA5": "c.SPI1"}, latency=LONG)],
    )
    try:
        tap = system.tap("a.SPI1", SpiTap)
        held: list[int] = []
        for step in range(1, 21):
            asyncio.run(to(system, VirtualTime(us(100 * step).ps)))
            held.append(tap.available)
        seen = tap.frames()
    finally:
        system.close()
    assert 1 in held
    assert [(f.slave, f.mosi, f.dropped) for f in seen] == [
        ("c.SPI1", 0x10, False),
        ("b.SPI1", 0x30, False),
        ("c.SPI1", 0x30, False),
    ]
    assert seen[0].at.ps < seen[1].at.ps == seen[2].at.ps
    assert seen[0].reply.ps > seen[1].reply.ps


def test_the_ss_output_selects_its_slave_and_the_tap_shows_no_pin(
    u575: popcorn.PartDescription,
) -> None:
    a = spi_member(u575, MASTER, "a", [op(HARD_NSS, 1), wait_us(50), send(2, 0x10)])
    b = spi_member(u575, SLAVE, "b")
    system = System({"a": a, "b": b}, [bus_of(slaves={"a.NSS": "b.SPI1"})])
    try:
        asyncio.run(to(system, END))
        seen = system.tap("a.SPI1", SpiTap).frames()
        assert [(f.cs, f.mosi, f.miso, f.dropped) for f in seen] == [
            (None, 0x10, 0xA0, False),
            (None, 0x11, 0x20, False),
        ]
    finally:
        system.close()


def test_a_slave_reset_under_a_frame_answers_all_ones_and_the_tap_marks_it(
    u575: popcorn.PartDescription,
) -> None:
    runs: list[list[SpiTapFrame]] = []
    for slice_instructions, third in VARIANT_VALUES:
        a = spi_member(u575, MASTER, "a", master_script(), slice_instructions=slice_instructions)
        b = spi_member(u575, SLAVE, "b", slice_instructions=slice_instructions)
        links: list[SpiBus | UartLink] = [bus_of()]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        system = System({"a": a, "b": b}, links, slice_instructions=slice_instructions)
        try:
            tap = system.tap("a.SPI1", SpiTap)
            asyncio.run(to(system, VirtualTime(us(143).ps)))
            first = tap.frames()
            system.reset("b", popcorn.ResetKind.SOFTWARE)
            asyncio.run(to(system, END))
            runs.append(first + tap.frames())
        finally:
            system.close()
    assert all(run == runs[0] for run in runs[1:])
    first = runs[0][0]
    assert (first.dropped, first.miso, first.reply.ps) == (
        True,
        0xFF,
        first.at.ps + L.ps + FRAME.ps,
    )


REFUSED: list[tuple[str, dict[str, object], str]] = [
    ("slave names no member", {"slaves": {"a.PA4": "z.SPI1"}}, "links[0].slaves['a.PA4']"),
    (
        "master names no member",
        {"master": "z.SPI1", "slaves": {"z.PA4": "b.SPI1"}},
        "links[0].master",
    ),
    ("slave on the master's member", {"slaves": {"a.PA4": "a.SPI2"}}, "links[0].slaves['a.PA4']"),
    ("select of another member", {"slaves": {"b.PA4": "b.SPI1"}}, "links[0].slaves['b.PA4']"),
    ("select not a pin", {"slaves": {"a.CS": "b.SPI1"}}, "links[0].slaves['a.CS']"),
    ("select pin above 15", {"slaves": {"a.PA16": "b.SPI1"}}, "links[0].slaves['a.PA16']"),
    ("select not a str", {"slaves": {4: "b.SPI1"}}, "links[0].slaves[4]"),
    ("no slave", {"slaves": {}}, "links[0].slaves"),
    ("latency 0", {"latency": us(0)}, "links[0].latency"),
    ("latency not a duration", {"latency": 20}, "links[0].latency"),
    ("slave not a str", {"slaves": {"a.PA4": 3}}, "links[0].slaves['a.PA4']"),
    (
        "one pin under two spellings",
        {"slaves": {"a.PA4": "b.SPI1", "a.PA04": "b.SPI2"}},
        "links[0].slaves['a.PA04']",
    ),
]


@pytest.mark.parametrize(("why", "changes", "key"), REFUSED, ids=[row[0] for row in REFUSED])
def test_a_bus_description_with_a_bad_field_is_refused_naming_its_key(
    why: str, changes: dict[str, object], key: str
) -> None:
    member = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(SPI_LL)])
    members = {"a": member, "b": member}
    with pytest.raises(ConfigError) as caught:
        SystemDescription(members=members, links=[bus_of(**changes)])
    assert caught.value.key == key, why
    assert SystemDescription(members=members, links=[bus_of()]).quantum == L


@pytest.mark.parametrize(("why", "changes", "key"), REFUSED, ids=[row[0] for row in REFUSED])
def test_a_system_refuses_a_bad_bus_field_naming_its_key_then_builds_the_fixed_bus(
    u575: popcorn.PartDescription, why: str, changes: dict[str, object], key: str
) -> None:
    a = spi_member(u575, MASTER, "a", master_script())
    b = spi_member(u575, SLAVE, "b")
    with pytest.raises(ConfigError) as caught:
        System({"a": a, "b": b}, [bus_of(**changes)])
    assert caught.value.key == key, why
    System({"a": a, "b": b}, [bus_of()]).close()


def test_an_spi_in_two_buses_or_a_bus_and_a_uart_link_is_refused() -> None:
    member = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(SPI_LL)])
    members = {"a": member, "b": member, "c": member}
    twice = [bus_of(), bus_of(slaves={"a.PA5": "b.SPI1"}, master="a.SPI2")]
    with pytest.raises(ConfigError) as caught:
        SystemDescription(members=members, links=twice)
    assert caught.value.key == "links[1].slaves['a.PA5']"
    master_twice = [bus_of(), bus_of(slaves={"a.PA5": "c.SPI1"})]
    with pytest.raises(ConfigError) as caught:
        SystemDescription(members=members, links=master_twice)
    assert caught.value.key == "links[1].master"
    with_uart = [
        bus_of(),
        UartLink("a.USART1", "b.SPI1", latency=L, baud=460_800),
    ]
    with pytest.raises(ConfigError) as caught:
        SystemDescription(members=members, links=with_uart)
    assert caught.value.key == "links[1].b"
    fixed = [bus_of(), UartLink("a.USART1", "b.USART1", latency=L, baud=460_800)]
    SystemDescription(members=members, links=fixed)
    assert SystemDescription(
        members=members, links=[bus_of(), bus_of(master="a.SPI2", slaves={"a.PA5": "c.SPI1"})]
    )


def test_an_spi_reached_by_its_alias_in_two_buses_is_refused_by_the_system(
    u575: popcorn.PartDescription,
) -> None:
    a = spi_member(u575, MASTER, "a", master_script())
    b = spi_member(u575, SLAVE, "b")
    c = spi_member(u575, SLAVE, "c")
    twice = [bus_of(), bus_of(master="a.SPI2", slaves={"a.PA5": "b.SEC_SPI1"})]
    with pytest.raises(ConfigError) as caught:
        System({"a": a, "b": b}, twice)
    assert caught.value.key == "links[1].slaves['a.PA5']"
    assert "b.SPI1 is in two links" in str(caught.value)
    master_twice = [bus_of(), bus_of(master="a.SEC_SPI1", slaves={"a.PA5": "c.SPI1"})]
    with pytest.raises(ConfigError) as caught:
        System({"a": a, "b": b, "c": c}, master_twice)
    assert caught.value.key == "links[1].master"
    system = System(
        {"a": a, "b": b, "c": c}, [bus_of(), bus_of(master="a.SPI2", slaves={"a.PA5": "c.SPI1"})]
    )
    system.close()


@pytest.mark.parametrize("slaves", [["a.PA4"], [("a.PA4", "b.SPI1")], "a.PA4"])
def test_a_bus_whose_slaves_are_no_mapping_is_refused_at_construction(slaves: object) -> None:
    with pytest.raises(ConfigError) as caught:
        bus_of(slaves=slaves)
    assert caught.value.key == "slaves"


def test_a_bus_is_a_frozen_hashable_value() -> None:
    one = bus_of()
    assert one == bus_of()
    assert hash(one) == hash(bus_of())
    assert one != bus_of(latency=us(21))
    with pytest.raises(TypeError):
        cast(Any, one.slaves)["a.PA5"] = "b.SPI2"


def test_a_system_of_a_bus_checks_the_machines_and_leaves_them_free_when_it_refuses(
    u575: popcorn.PartDescription,
) -> None:
    a = spi_member(u575, MASTER, "a", master_script())
    b = spi_member(u575, SLAVE, "b")
    c = spi_member(u575, SLAVE, "c", devices={"SPI1": [SpiTarget()]})
    d = spi_member(u575, SLAVE, "d", devices={"SPI1": [SpiController(sck_hz=1_000_000)]})
    with pytest.raises(ConfigError) as caught:
        System({"a": a, "b": b, "c": c}, [bus_of(slaves={"a.PA4": "b.SPI1", "a.PA5": "c.SPI1"})])
    assert caught.value.key == "links[0].slaves['a.PA5']"
    with pytest.raises(ConfigError) as caught:
        System({"a": a, "d": d}, [bus_of(slaves={"a.PA4": "d.SPI1"})])
    assert caught.value.key == "links[0].slaves['a.PA4']"
    with pytest.raises(ConfigError) as caught:
        System({"a": a, "b": b}, [bus_of(slaves={"a.PA4": "b.USART1"})])
    assert caught.value.key == "links[0].slaves['a.PA4']"
    with pytest.raises(ConfigError) as caught:
        System({"a": a, "b": b}, [bus_of(slaves={"a.PX9": "b.SPI1"})])
    assert caught.value.key == "links[0].slaves['a.PX9']"
    with pytest.raises(ConfigError) as caught:
        System({"a": a, "b": b}, [bus_of(master="a.USART1")])
    assert caught.value.key == "links[0].master"
    system = System({"a": a, "b": b}, [bus_of()])
    try:
        asyncio.run(to(system, END))
        assert len(system.tap("a.SPI1", SpiTap).frames()) == 3
    finally:
        system.close()


def test_a_master_with_a_board_controller_is_refused_and_one_with_board_devices_is_not(
    u575: popcorn.PartDescription,
) -> None:
    controlled = spi_member(u575, MASTER, "a", devices={"SPI1": [SpiController(sck_hz=1_000_000)]})
    b = spi_member(u575, SLAVE, "b")
    with pytest.raises(ConfigError) as caught:
        System({"a": controlled, "b": b}, [bus_of()])
    assert caught.value.key == "links[0].master"
    with_device = spi_member(u575, MASTER, "a", devices={"SPI1": [SpiTarget(cs="PA6")]})
    system = System({"a": with_device, "b": b}, [bus_of()])
    system.close()


@pytest.fixture(scope="module")
def zgtx() -> popcorn.PartDescription:
    """STM32U575ZGTx, LQFP144, which bonds no pin of port I."""
    value = os.environ.get(PIN_DATA_VARIABLE)
    if not value:
        pytest.skip(f"set {PIN_DATA_VARIABLE} to a STM32_open_pin_data checkout")
    return popcorn.parse_description(build_text("STM32U575ZGTx", pin_data=Path(value)), "zgtx")


def test_a_chip_select_on_an_unbonded_pad_is_refused_with_the_board_device_s_key(
    zgtx: popcorn.PartDescription, u575: popcorn.PartDescription
) -> None:
    with pytest.raises(ConfigError) as device:
        popcorn.Machine(zgtx, devices={"SPI1": [SpiTarget(cs="PI0")]})
    a = spi_member(zgtx, MASTER, "a", master_script())
    b = spi_member(u575, SLAVE, "b")
    with pytest.raises(ConfigError) as bus:
        System({"a": a, "b": b}, [bus_of(slaves={"a.PI0": "b.SPI1"})])
    assert bus.value.key == device.value.key == "pin"
    assert bus.value.expected == device.value.expected
    system = System({"a": a, "b": b}, [bus_of()])
    system.close()


def test_the_tap_kind_is_checked_at_the_call(pair: Build, u575: popcorn.PartDescription) -> None:
    system = pair()
    with pytest.raises(TypeError, match="has an SpiTap, not a UartTap"):
        system.tap("a.SPI1", UartTap)
    with pytest.raises(ValueError, match="is no end of a link"):
        system.tap("a.USART1")
    assert isinstance(system.tap("a.SPI1", SpiTap), SpiTap)
    with pytest.raises(TypeError, match=r"'a\.SPI1' has an SpiTap, pass SpiTap"):
        system.tap("a.SPI1")


def test_a_uart_tap_keeps_its_kind_next_to_an_spi_bus(u575: popcorn.PartDescription) -> None:
    a = spi_member(u575, MASTER, "a", master_script())
    b = spi_member(u575, SLAVE, "b")
    links: list[SpiBus | UartLink] = [
        bus_of(),
        UartLink("a.USART2", "b.USART2", latency=us(30), baud=460_800),
    ]
    system = System({"a": a, "b": b}, links)
    try:
        assert isinstance(system.tap("a.USART2"), UartTap)
        assert isinstance(system.tap("a.SPI1", SpiTap), SpiTap)
        with pytest.raises(TypeError, match="has a UartTap, not an SpiTap"):
            system.tap("a.USART2", SpiTap)
        assert system.quantum == us(20)
    finally:
        system.close()


def test_a_restore_then_a_run_equals_the_run_after_the_snapshot(pair: Build) -> None:
    seen: list[tuple[list[SpiTapFrame], tuple[list[int], list[int]]]] = []
    for slice_instructions, third in VARIANT_VALUES:
        system = pair(slice_instructions, third)
        tap = system.tap("a.SPI1", SpiTap)
        asyncio.run(to(system, VirtualTime(us(130).ps)))
        taken = system.snapshot()
        asyncio.run(to(system, END))
        once = (tap.frames(), observed(system))
        system.restore(taken)
        assert tap.available == 0
        asyncio.run(to(system, END))
        assert (tap.frames(), observed(system)) == once
        seen.append(once)
    assert seen[1:] == seen[:1] * 3


@pytest.mark.parametrize("pulled", [False, True], ids=["native", "buffered"])
def test_a_restore_drops_the_frames_a_tap_held_unread(pair: Build, pulled: bool) -> None:
    system = pair()
    tap = system.tap("a.SPI1", SpiTap)
    asyncio.run(to(system, VirtualTime(us(130).ps)))
    taken = system.snapshot()
    asyncio.run(to(system, END))
    if pulled:
        assert tap.available == 3
    system.restore(taken)
    assert (tap.available, tap.dropped) == (0, 0)
    assert tap.frames() == []
    asyncio.run(to(system, END))
    assert len(tap.frames()) == 3


def test_the_tap_keeps_its_limit_and_counts_what_it_dropped(
    pair: Build, monkeypatch: pytest.MonkeyPatch
) -> None:
    from popcorn._system import taps

    monkeypatch.setattr(taps, "_TAP_KEEP", 2)
    system = pair()
    tap = system.tap("a.SPI1", SpiTap)
    asyncio.run(to(system, END))
    assert (tap.available, tap.dropped) == (2, 1)
    assert [f.mosi for f in tap.frames()] == [0x11, 0x12]
    assert (tap.available, tap.dropped) == (0, 1)


def test_a_closed_system_s_tap_stays_readable_and_the_system_is_collected(
    u575: popcorn.PartDescription,
) -> None:
    a = spi_member(u575, MASTER, "a", master_script())
    b = spi_member(u575, SLAVE, "b")
    system = System({"a": a, "b": b}, [bus_of()])
    tap = system.tap("a.SPI1", SpiTap)
    asyncio.run(to(system, END))
    system.close()
    frames = tap.frames()
    assert len(frames) == 3
    assert tap.available == 0
    held = weakref.ref(system)
    machines = [weakref.ref(a), weakref.ref(b)]
    del system, a, b, tap
    assert collected(held)
    assert all(collected(machine) for machine in machines)


def test_a_system_collected_unclosed_leaves_no_machine_alive(
    u575: popcorn.PartDescription,
) -> None:
    a = spi_member(u575, MASTER, "a", master_script())
    b = spi_member(u575, SLAVE, "b")
    system = System({"a": a, "b": b}, [bus_of()])
    asyncio.run(to(system, VirtualTime(us(100).ps)))
    held = weakref.ref(system)
    del system
    assert collected(held)
    machines = [weakref.ref(a), weakref.ref(b)]
    del a, b
    assert all(collected(machine) for machine in machines)


def test_a_bus_run_takes_no_gil_and_a_tap_read_runs_no_user_code(pair: Build) -> None:
    system = pair()
    a, b = system.members["a"], system.members["b"]
    handles = [m._handle for m in (a, b)]  # pyright: ignore[reportPrivateUsage]
    before = [handle.gil_entries() for handle in handles]
    asyncio.run(to(system, END))
    system.tap("a.SPI1", SpiTap).frames()
    gc.collect()
    assert [handle.gil_entries() for handle in handles] == before


def test_uart_only_systems_are_unchanged_by_the_lane_counter(u575: popcorn.PartDescription) -> None:
    from .linked import links, member

    a, b = member(u575, "link_tx", "a"), member(u575, "link_rx", "b")
    system = System({"a": a, "b": b}, links(None))
    try:
        asyncio.run(to(system, VirtualTime(ms(1).ps)))
        assert isinstance(system.tap("a.USART1"), UartTap)
        assert len(system.tap("a.USART1").frames()) > 0
    finally:
        system.close()
