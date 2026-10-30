"""`popcorn.I2CBus` and `I2CTap`: an I2C controller and targets on other members of a linked
system, built from the i2c_ll link firmware."""

from __future__ import annotations

import asyncio
import gc
import weakref
from collections.abc import Callable, Iterator, Mapping, Sequence
from pathlib import Path
from typing import Any, cast

import popcorn
import pytest
from popcorn import (
    ConfigError,
    I2CBus,
    I2CTap,
    I2CTapPhase,
    SpiBus,
    SpiTap,
    System,
    SystemDescription,
    UartLink,
    UartTap,
)
from popcorn.board import I2CController, I2CPhase, I2CTarget
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .linked import VARIANT_VALUES, VARIANTS, in_flight, to
from .test_system_state import encoded

I2C_LL = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "i2c_ll.elf"
AREA = 0x20080000
LOG_AT = 296
MAGIC = 0x12C11B
CONTROLLER = 1
TARGET = 2
WRITE = 1
READ = 2
WRITE_SR = 3
WAIT_US = 4
RESET = 5
BOOT = 0xB0000000
STOPPED = 0x5709
OWN = 0x42
L = us(20)
PERIOD_PS = 2_500_000
ADDRESS_PS = 10 * PERIOD_PS
BYTE_PS = 9 * PERIOD_PS
END = VirtualTime(ms(1).ps)
RESET_AT = VirtualTime(us(225).ps)
SNAPSHOT_AT = VirtualTime(us(310).ps)

Build = Callable[..., System]


def op(code: int, arg: int = 0) -> int:
    return code << 24 | arg


def write(address: int, count: int, first: int) -> int:
    return op(WRITE, address << 16 | count << 8 | first)


def write_sr(address: int, count: int, first: int) -> int:
    return op(WRITE_SR, address << 16 | count << 8 | first)


def read(address: int, count: int) -> int:
    return op(READ, address << 16 | count << 8)


def wait_us(micros: int) -> int:
    return op(WAIT_US, micros)


def addressed(address: int, reading: bool = False) -> int:
    return 0xADD00000 | address << 1 | int(reading)


def data(byte: int) -> int:
    return 0xDA7A0000 | byte


def written(address: int, sent: int, nack: bool = False) -> int:
    return 0x57000000 | int(nack) << 16 | address << 8 | sent


def got(byte: int) -> int:
    return 0x5D000000 | byte


def read_done(address: int, nack: bool = False) -> int:
    return 0x5E000000 | int(nack) << 16 | address << 8


def i2c_member(
    part: popcorn.PartDescription,
    entry: int,
    name: str,
    script: Sequence[int] = (),
    *,
    own: int = OWN,
    first_tx: int = 0,
    slice_instructions: int = 1_000_000,
    devices: Mapping[str, Sequence[object]] | None = None,
) -> popcorn.Machine:
    """An i2c_ll machine named `name` running the link firmware as controller or target, `script`
    its steps, `own` the target's address, `first_tx` the first byte it sends when not 0."""
    m = popcorn.Machine(
        part,
        image=popcorn.Image.elf(I2C_LL),
        name=name,
        slice_instructions=slice_instructions,
        devices=cast(Any, devices),
    )
    m.reset()
    stop = asyncio.run(m.run(until="main", budget=1_000_000))
    assert stop.reason is popcorn.StopReason.ADDRESS, stop
    words = m.mem.u32
    head = (MAGIC, entry, 0, 0, 0, first_tx, own, 0, 0, 0)
    for index, value in enumerate(head):
        words[AREA + 4 * index] = value
    for index in range(64):
        words[AREA + 40 + 4 * index] = script[index] if index < len(script) else 0
    m.reg.pc = m.sym("link_run")
    return m


def logged(m: popcorn.Machine) -> list[int]:
    """The firmware's link log words."""
    count = m.mem.u32[AREA + 12]
    return [m.mem.u32[AREA + LOG_AT + 4 * index] for index in range(min(count, 256))]


def controller_script() -> list[int]:
    return [wait_us(50), write(OWN, 3, 0x10), read(OWN, 2)]


def bus_of(**changes: object) -> I2CBus:
    values: dict[str, object] = {"ports": ["a.I2C1", "b.I2C1"], "latency": L, **changes}
    return I2CBus(**values)  # pyright: ignore[reportArgumentType]


@pytest.fixture
def pair(u575: popcorn.PartDescription) -> Iterator[Build]:
    """Builds `a`, the controller, writing to and reading from `b` on 0x42, over `bus`."""
    made: list[System] = []

    def build(
        slice_instructions: int = 1_000_000,
        third: VirtualDuration | None = None,
        *,
        script: Sequence[int] | None = None,
        bus: I2CBus | None = None,
    ) -> System:
        a = i2c_member(
            u575,
            CONTROLLER,
            "a",
            controller_script() if script is None else script,
            slice_instructions=slice_instructions,
        )
        b = i2c_member(u575, TARGET, "b", slice_instructions=slice_instructions)
        links: list[I2CBus | UartLink] = [bus or bus_of()]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        system = System({"a": a, "b": b}, links, slice_instructions=slice_instructions)
        made.append(system)
        return system

    yield build
    for one in made:
        one.close()


def observed(system: System) -> tuple[list[int], ...]:
    return tuple(logged(m) for m in system.members.values())


WRITE_THEN_READ = [
    (I2CPhase.ADDRESS, OWN << 1, True),
    (I2CPhase.WRITE, 0x10, True),
    (I2CPhase.WRITE, 0x11, True),
    (I2CPhase.WRITE, 0x12, True),
    (I2CPhase.ADDRESS, OWN << 1 | 1, True),
    (I2CPhase.READ, 0xB0, True),
    (I2CPhase.READ, 0xB1, False),
]


WRITE_THEN_READ_BEFORE_SNAPSHOT = [
    (I2CPhase.ADDRESS, OWN << 1),
    (I2CPhase.WRITE, 0x10),
]


def test_a_bus_gives_the_controller_the_target_s_answers_and_the_tap_every_phase_at_any_quantum(
    pair: Build,
) -> None:
    results: list[tuple[list[I2CTapPhase], tuple[list[int], ...]]] = []
    for slice_instructions, third in VARIANT_VALUES:
        system = pair(slice_instructions, third)
        tap = system.tap("a.I2C1", I2CTap)
        asyncio.run(to(system, END))
        results.append((tap.phases(), observed(system)))
    assert all(result == results[0] for result in results[1:])
    seen, (controller, target) = results[0]
    assert [(p.phase, p.byte, p.ack) for p in seen] == WRITE_THEN_READ
    assert all(p.target == "b.I2C1" and not p.dropped for p in seen)
    assert [p.unit for p in seen] == sorted({p.unit for p in seen})
    for phase in seen:
        if phase.phase is I2CPhase.ADDRESS:
            assert phase.answer.ps > phase.at.ps + L.ps + ADDRESS_PS
        else:
            assert phase.answer.ps == phase.at.ps + L.ps + BYTE_PS
    assert controller[1:] == [written(OWN, 3), got(0xB0), got(0xB1), read_done(OWN)]
    assert target[1:] == [
        addressed(OWN),
        data(0x10),
        data(0x11),
        data(0x12),
        STOPPED,
        addressed(OWN, True),
        0x4AC0,
        STOPPED,
    ]


def test_the_tap_of_any_end_is_one_object_and_shows_the_bus(pair: Build) -> None:
    system = pair()
    tap = system.tap("a.I2C1", I2CTap)
    assert system.tap("b.I2C1", I2CTap) is tap
    assert tap.ends == ("a.I2C1", "b.I2C1")
    assert repr(tap) == "I2CTap(a.I2C1 -> b.I2C1)"
    assert (tap.available, tap.dropped) == (0, 0)


def free_run(
    system: System, running: bool
) -> tuple[list[I2CTapPhase], tuple[list[int], ...], tuple[object, ...]]:
    """The tap and the logs at 1 ms of system time, reached by a free run to 0.5 ms when
    `running` and by TIME stops otherwise."""
    tap = system.tap("a.I2C1", I2CTap)
    b = system.members["b"]

    async def free() -> None:
        async with system.running():
            await b.elapse(us(100))
            await in_flight(system)
            await b.elapse(us(400))

    if running:
        asyncio.run(free())
    asyncio.run(to(system, END))
    return tap.phases(), observed(system), encoded(system)


@pytest.mark.parametrize(("slice_instructions", "third"), VARIANTS)
def test_a_free_run_gives_the_phases_and_logs_of_the_parked_run(
    pair: Build, slice_instructions: int, third: VirtualDuration | None
) -> None:
    parked = free_run(pair(slice_instructions, third), False)
    running = free_run(pair(slice_instructions, third), True)
    assert running == parked
    assert len(parked[0]) == len(WRITE_THEN_READ)


def two_targets(
    u575: popcorn.PartDescription, script: Sequence[int], slice_instructions: int = 1_000_000
) -> tuple[dict[str, popcorn.Machine], I2CBus]:
    machines = {
        "a": i2c_member(u575, CONTROLLER, "a", script, slice_instructions=slice_instructions),
        "b": i2c_member(u575, TARGET, "b", slice_instructions=slice_instructions),
        "c": i2c_member(
            u575, TARGET, "c", own=0x50, first_tx=0xC0, slice_instructions=slice_instructions
        ),
    }
    return machines, bus_of(ports=["a.I2C1", "b.I2C1", "c.I2C1"])


TWO_TARGET_SCRIPT = [wait_us(50), write(OWN, 1, 0x20), read(0x50, 1), write(0x33, 1, 0x30)]


def test_two_targets_each_get_their_own_transfers_and_every_address(
    u575: popcorn.PartDescription,
) -> None:
    runs: list[tuple[list[I2CTapPhase], tuple[list[int], ...]]] = []
    for slice_instructions, third in VARIANT_VALUES:
        machines, bus = two_targets(u575, TWO_TARGET_SCRIPT, slice_instructions)
        links: list[I2CBus | UartLink] = [bus]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        system = System(machines, links, slice_instructions=slice_instructions)
        try:
            tap = system.tap("c.I2C1", I2CTap)
            asyncio.run(to(system, END))
            runs.append((tap.phases(), observed(system)))
        finally:
            system.close()
    assert all(run == runs[0] for run in runs[1:])
    seen, (controller, b, c) = runs[0]
    assert [(p.target, p.phase, p.byte, p.ack) for p in seen] == [
        ("b.I2C1", I2CPhase.ADDRESS, OWN << 1, True),
        ("c.I2C1", I2CPhase.ADDRESS, OWN << 1, False),
        ("b.I2C1", I2CPhase.WRITE, 0x20, True),
        ("b.I2C1", I2CPhase.ADDRESS, 0x50 << 1 | 1, False),
        ("c.I2C1", I2CPhase.ADDRESS, 0x50 << 1 | 1, True),
        ("c.I2C1", I2CPhase.READ, 0xC0, False),
        ("b.I2C1", I2CPhase.ADDRESS, 0x33 << 1, False),
        ("c.I2C1", I2CPhase.ADDRESS, 0x33 << 1, False),
    ]
    assert [p.at.ps for p in seen] == sorted(p.at.ps for p in seen)
    assert controller[1:] == [written(OWN, 1), got(0xC0), read_done(0x50), written(0x33, 0, True)]
    assert b[1:] == [addressed(OWN), data(0x20), STOPPED]
    assert c[1:] == [addressed(0x50, True), 0x4AC0, STOPPED]


def test_answers_of_two_targets_to_one_address_come_out_by_target_across_paused_reads(
    u575: popcorn.PartDescription,
) -> None:
    runs: list[list[I2CTapPhase]] = []
    for slice_instructions, third in VARIANT_VALUES:
        machines, bus = two_targets(u575, TWO_TARGET_SCRIPT, slice_instructions)
        links: list[I2CBus | UartLink] = [bus]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        system = System(machines, links, slice_instructions=slice_instructions)
        try:
            tap = system.tap("a.I2C1", I2CTap)
            for micros in range(150, 400, 3):
                asyncio.run(to(system, VirtualTime(us(micros).ps)))
                assert tap.available >= 0
            asyncio.run(to(system, END))
            runs.append(tap.phases())
        finally:
            system.close()
    assert all(run == runs[0] for run in runs[1:])
    first, second = runs[0][:2]
    assert (first.target, second.target) == ("b.I2C1", "c.I2C1")
    assert first.at == second.at
    assert first.answer.ps > second.answer.ps


def test_phases_of_one_target_leave_the_others_in_the_tap(u575: popcorn.PartDescription) -> None:
    machines, bus = two_targets(u575, TWO_TARGET_SCRIPT)
    system = System(machines, [bus])
    try:
        tap = system.tap("a.I2C1", I2CTap)
        asyncio.run(to(system, END))
        of_c = tap.phases(target="c.I2C1")
        assert {p.target for p in of_c} == {"c.I2C1"}
        assert len(of_c) == 4
        rest = tap.phases()
        assert {p.target for p in rest} == {"b.I2C1"}
        assert len(rest) == 4
        with pytest.raises(ValueError, match="is no target of"):
            tap.phases(target="a.I2C1")
        with pytest.raises(TypeError, match="target must be a str"):
            tap.phases(target=cast(Any, 3))
    finally:
        system.close()


def test_a_target_reset_under_a_transfer_answers_nack_and_the_tap_marks_it(
    u575: popcorn.PartDescription,
) -> None:
    runs: list[tuple[list[I2CTapPhase], tuple[list[int], ...]]] = []
    for slice_instructions, third in VARIANT_VALUES:
        a = i2c_member(
            u575,
            CONTROLLER,
            "a",
            [wait_us(50), write(OWN, 3, 0x10)],
            slice_instructions=slice_instructions,
        )
        b = i2c_member(u575, TARGET, "b", slice_instructions=slice_instructions)
        links: list[I2CBus | UartLink] = [bus_of()]
        if third is not None:
            links.append(UartLink("a.USART2", "b.USART2", latency=third, baud=460_800))
        system = System({"a": a, "b": b}, links, slice_instructions=slice_instructions)
        try:
            tap = system.tap("a.I2C1", I2CTap)
            asyncio.run(to(system, RESET_AT))
            first = tap.phases()
            system.reset("b", popcorn.ResetKind.SOFTWARE)
            asyncio.run(to(system, END))
            runs.append((first + tap.phases(), observed(system)))
        finally:
            system.close()
    assert all(run == runs[0] for run in runs[1:])
    seen = runs[0][0]
    dropped = [p for p in seen if p.dropped]
    assert [(p.unit, p.phase, p.byte, p.ack) for p in dropped] == [(2, I2CPhase.WRITE, 0x10, False)]
    assert dropped[0].answer.ps == dropped[0].at.ps + L.ps + BYTE_PS


REFUSED: list[tuple[str, dict[str, object], str]] = [
    ("target names no member", {"ports": ["a.I2C1", "z.I2C1"]}, "links[0].ports[1]"),
    ("controller names no member", {"ports": ["z.I2C1", "b.I2C1"]}, "links[0].ports[0]"),
    ("target on the controller's member", {"ports": ["a.I2C1", "a.I2C2"]}, "links[0].ports[1]"),
    ("no target", {"ports": ["a.I2C1"]}, "links[0].ports"),
    ("no port", {"ports": []}, "links[0].ports"),
    ("latency 0", {"latency": us(0)}, "links[0].latency"),
    ("latency not a duration", {"latency": 20}, "links[0].latency"),
    ("target not a str", {"ports": ["a.I2C1", 3]}, "links[0].ports[1]"),
    ("controller not a str", {"ports": [3, "b.I2C1"]}, "links[0].ports[0]"),
    ("one target twice", {"ports": ["a.I2C1", "b.I2C1", "b.I2C1"]}, "links[0].ports[2]"),
]


@pytest.mark.parametrize(("why", "changes", "key"), REFUSED, ids=[row[0] for row in REFUSED])
def test_a_bus_description_with_a_bad_field_is_refused_naming_its_key(
    why: str, changes: dict[str, object], key: str
) -> None:
    member = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(I2C_LL)])
    members = {"a": member, "b": member}
    with pytest.raises(ConfigError) as caught:
        SystemDescription(members=members, links=[bus_of(**changes)])
    assert caught.value.key == key, why
    assert SystemDescription(members=members, links=[bus_of()]).quantum == L


@pytest.mark.parametrize(("why", "changes", "key"), REFUSED, ids=[row[0] for row in REFUSED])
def test_a_system_refuses_a_bad_bus_field_naming_its_key_then_builds_the_fixed_bus(
    u575: popcorn.PartDescription, why: str, changes: dict[str, object], key: str
) -> None:
    a = i2c_member(u575, CONTROLLER, "a", controller_script())
    b = i2c_member(u575, TARGET, "b")
    with pytest.raises(ConfigError) as caught:
        System({"a": a, "b": b}, [bus_of(**changes)])
    assert caught.value.key == key, why
    System({"a": a, "b": b}, [bus_of()]).close()


@pytest.mark.parametrize("ports", ["a.I2C1", b"a.I2C1", 5, None])
def test_a_bus_whose_ports_are_no_sequence_is_refused_at_construction(ports: object) -> None:
    with pytest.raises(ConfigError) as caught:
        bus_of(ports=ports)
    assert caught.value.key == "ports"


def test_a_bus_is_a_frozen_hashable_value() -> None:
    one = bus_of()
    assert one == bus_of(ports=("a.I2C1", "b.I2C1"))
    assert hash(one) == hash(bus_of())
    assert one != bus_of(latency=us(21))
    assert one.ports == ("a.I2C1", "b.I2C1")
    with pytest.raises(AttributeError):
        cast(Any, one).ports = ("a.I2C1",)


def test_an_i2c_in_two_buses_or_a_bus_and_another_link_is_refused() -> None:
    member = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(I2C_LL)])
    members = {"a": member, "b": member, "c": member}
    twice = [bus_of(), bus_of(ports=["a.I2C2", "b.I2C1"])]
    with pytest.raises(ConfigError) as caught:
        SystemDescription(members=members, links=twice)
    assert caught.value.key == "links[1].ports[1]"
    controller_twice = [bus_of(), bus_of(ports=["a.I2C1", "c.I2C1"])]
    with pytest.raises(ConfigError) as caught:
        SystemDescription(members=members, links=controller_twice)
    assert caught.value.key == "links[1].ports[0]"
    with_uart = [bus_of(), UartLink("a.USART1", "b.I2C1", latency=L, baud=460_800)]
    with pytest.raises(ConfigError) as caught:
        SystemDescription(members=members, links=with_uart)
    assert caught.value.key == "links[1].b"
    with_spi = [bus_of(), SpiBus(master="a.I2C1", slaves={"a.PA4": "c.SPI1"}, latency=L)]
    with pytest.raises(ConfigError) as caught:
        SystemDescription(members=members, links=with_spi)
    assert caught.value.key == "links[1].master"
    fixed = [
        bus_of(),
        bus_of(ports=["a.I2C2", "c.I2C1"]),
        UartLink("a.USART1", "b.USART1", latency=L, baud=460_800),
        SpiBus(master="a.SPI1", slaves={"a.PA4": "c.SPI1"}, latency=L),
    ]
    assert SystemDescription(members=members, links=fixed).quantum == L


def test_an_i2c_reached_by_its_alias_in_two_buses_is_refused_by_the_system(
    u575: popcorn.PartDescription,
) -> None:
    a = i2c_member(u575, CONTROLLER, "a", controller_script())
    b = i2c_member(u575, TARGET, "b")
    c = i2c_member(u575, TARGET, "c")
    twice = [bus_of(), bus_of(ports=["a.I2C2", "b.SEC_I2C1"])]
    with pytest.raises(ConfigError) as caught:
        System({"a": a, "b": b}, twice)
    assert caught.value.key == "links[1].ports[1]"
    assert "b.I2C1 is in two links" in str(caught.value)
    controller_twice = [bus_of(), bus_of(ports=["a.SEC_I2C1", "c.I2C1"])]
    with pytest.raises(ConfigError) as caught:
        System({"a": a, "b": b, "c": c}, controller_twice)
    assert caught.value.key == "links[1].ports[0]"
    System({"a": a, "b": b, "c": c}, [bus_of(), bus_of(ports=["a.I2C2", "c.I2C1"])]).close()


def test_a_system_of_a_bus_checks_the_board_of_each_end_and_leaves_the_machines_free(
    u575: popcorn.PartDescription,
) -> None:
    a = i2c_member(u575, CONTROLLER, "a", controller_script())
    b = i2c_member(u575, TARGET, "b")
    with_target = i2c_member(u575, TARGET, "c", devices={"I2C1": [I2CTarget(0x60)]})
    with_controller = i2c_member(
        u575, TARGET, "d", devices={"I2C1": [I2CController(scl_hz=400_000)]}
    )
    controlled = i2c_member(
        u575, CONTROLLER, "e", devices={"I2C1": [I2CController(scl_hz=400_000)]}
    )
    rows: list[tuple[dict[str, popcorn.Machine], list[str], str, str]] = [
        ({"a": a, "c": with_target}, ["a.I2C1", "c.I2C1"], "links[0].ports[1]", "no board target"),
        (
            {"a": a, "d": with_controller},
            ["a.I2C1", "d.I2C1"],
            "links[0].ports[1]",
            "no board controller",
        ),
        (
            {"e": controlled, "b": b},
            ["e.I2C1", "b.I2C1"],
            "links[0].ports[0]",
            "no board controller",
        ),
        ({"a": a, "b": b}, ["a.I2C1", "b.SPI1"], "links[0].ports[1]", "an I2C instance"),
        ({"a": a, "b": b}, ["a.USART1", "b.I2C1"], "links[0].ports[0]", "an I2C instance"),
        ({"a": a, "b": b}, ["a.I2C1", "b.I2C9"], "links[0].ports[1]", "an I2C instance"),
    ]
    for machines, ports, key, expected in rows:
        with pytest.raises(ConfigError) as caught:
            System(machines, [bus_of(ports=ports)])
        assert caught.value.key == key, ports
        assert expected in caught.value.expected, ports
    system = System({"a": a, "b": b}, [bus_of()])
    try:
        asyncio.run(to(system, END))
        assert len(system.tap("a.I2C1", I2CTap).phases()) == len(WRITE_THEN_READ)
    finally:
        system.close()


def test_a_controller_with_a_board_target_keeps_it_next_to_the_bus(
    u575: popcorn.PartDescription,
) -> None:
    a = i2c_member(u575, CONTROLLER, "a", controller_script(), devices={"I2C1": [I2CTarget(0x60)]})
    b = i2c_member(u575, TARGET, "b")
    system = System({"a": a, "b": b}, [bus_of()])
    try:
        asyncio.run(to(system, END))
        seen = system.tap("a.I2C1", I2CTap).phases()
        assert [(p.phase, p.byte, p.ack) for p in seen] == WRITE_THEN_READ
    finally:
        system.close()


def test_the_tap_kind_is_checked_at_the_call(pair: Build) -> None:
    system = pair()
    with pytest.raises(TypeError, match="has an I2CTap, not a UartTap"):
        system.tap("a.I2C1", UartTap)
    with pytest.raises(TypeError, match="has an I2CTap, not an SpiTap"):
        system.tap("a.I2C1", SpiTap)
    with pytest.raises(TypeError, match=r"'a\.I2C1' has an I2CTap, pass I2CTap"):
        system.tap("a.I2C1")
    with pytest.raises(ValueError, match="is no end of a link"):
        system.tap("a.I2C2", I2CTap)
    assert isinstance(system.tap("b.SEC_I2C1", I2CTap), I2CTap)


def test_a_uart_tap_keeps_its_kind_next_to_an_i2c_bus(u575: popcorn.PartDescription) -> None:
    a = i2c_member(u575, CONTROLLER, "a", controller_script())
    b = i2c_member(u575, TARGET, "b")
    links: list[I2CBus | UartLink] = [
        bus_of(),
        UartLink("a.USART2", "b.USART2", latency=us(30), baud=460_800),
    ]
    system = System({"a": a, "b": b}, links)
    try:
        assert isinstance(system.tap("a.USART2"), UartTap)
        with pytest.raises(TypeError, match="has a UartTap, not an I2CTap"):
            system.tap("a.USART2", I2CTap)
        assert system.quantum == L
    finally:
        system.close()


def test_a_restore_then_a_run_equals_the_run_after_the_snapshot(pair: Build) -> None:
    seen: list[tuple[list[I2CTapPhase], tuple[list[int], ...]]] = []
    for slice_instructions, third in VARIANT_VALUES:
        system = pair(slice_instructions, third)
        tap = system.tap("a.I2C1", I2CTap)
        asyncio.run(to(system, SNAPSHOT_AT))
        before = tap.phases()
        assert [(p.phase, p.byte) for p in before] == WRITE_THEN_READ_BEFORE_SNAPSHOT
        taken = system.snapshot()
        asyncio.run(to(system, END))
        once = (before + tap.phases(), observed(system))
        system.restore(taken)
        assert tap.available == 0
        asyncio.run(to(system, END))
        assert (before + tap.phases(), observed(system)) == once
        seen.append(once)
    assert seen[1:] == seen[:1] * 3
    assert [(p.phase, p.byte, p.ack) for p in seen[0][0]] == WRITE_THEN_READ


@pytest.mark.parametrize("pulled", [False, True], ids=["native", "buffered"])
def test_a_restore_drops_the_phases_a_tap_held_unread(pair: Build, pulled: bool) -> None:
    system = pair()
    tap = system.tap("a.I2C1", I2CTap)
    asyncio.run(to(system, VirtualTime(us(300).ps)))
    held = tap.available
    assert 0 < held < len(WRITE_THEN_READ)
    taken = system.snapshot()
    asyncio.run(to(system, END))
    if pulled:
        assert tap.available == len(WRITE_THEN_READ)
    system.restore(taken)
    assert (tap.available, tap.dropped) == (0, 0)
    assert tap.phases() == []
    asyncio.run(to(system, END))
    assert len(tap.phases()) == len(WRITE_THEN_READ) - held


def test_a_restore_drops_the_phases_of_a_tap_not_built_yet(pair: Build) -> None:
    system = pair()
    asyncio.run(to(system, VirtualTime(us(300).ps)))
    taken = system.snapshot()
    asyncio.run(to(system, END))
    system.restore(taken)
    asyncio.run(to(system, END))
    units = [p.unit for p in system.tap("a.I2C1", I2CTap).phases()]
    assert 0 < len(units) < len(WRITE_THEN_READ)
    assert units == sorted(set(units))


def test_the_tap_keeps_its_limit_and_counts_what_it_dropped(
    pair: Build, monkeypatch: pytest.MonkeyPatch
) -> None:
    from popcorn import _system

    monkeypatch.setattr(_system, "_TAP_KEEP", 2)
    system = pair()
    tap = system.tap("a.I2C1", I2CTap)
    asyncio.run(to(system, END))
    assert (tap.available, tap.dropped) == (2, len(WRITE_THEN_READ) - 2)
    assert [p.byte for p in tap.phases()] == [0xB0, 0xB1]
    assert (tap.available, tap.dropped) == (0, len(WRITE_THEN_READ) - 2)


def test_a_closed_system_s_tap_stays_readable_and_the_system_is_collected(
    u575: popcorn.PartDescription,
) -> None:
    a = i2c_member(u575, CONTROLLER, "a", controller_script())
    b = i2c_member(u575, TARGET, "b")
    system = System({"a": a, "b": b}, [bus_of()])
    tap = system.tap("a.I2C1", I2CTap)
    asyncio.run(to(system, END))
    system.close()
    assert len(tap.phases()) == len(WRITE_THEN_READ)
    assert tap.available == 0
    held = weakref.ref(system)
    machines = [weakref.ref(a), weakref.ref(b)]
    del system, a, b, tap
    gc.collect()
    assert held() is None
    assert all(machine() is None for machine in machines)


def test_a_system_collected_unclosed_leaves_no_machine_alive(
    u575: popcorn.PartDescription,
) -> None:
    a = i2c_member(u575, CONTROLLER, "a", controller_script())
    b = i2c_member(u575, TARGET, "b")
    system = System({"a": a, "b": b}, [bus_of()])
    asyncio.run(to(system, VirtualTime(us(150).ps)))
    held = weakref.ref(system)
    del system
    gc.collect()
    assert held() is None
    machines = [weakref.ref(a), weakref.ref(b)]
    del a, b
    gc.collect()
    assert all(machine() is None for machine in machines)


def test_a_closed_system_gives_the_i2cs_back_to_a_new_system(
    u575: popcorn.PartDescription,
) -> None:
    a = i2c_member(u575, CONTROLLER, "a", controller_script())
    b = i2c_member(u575, TARGET, "b")
    System({"a": a, "b": b}, [bus_of()]).close()
    gc.collect()
    system = System({"a": a, "b": b}, [bus_of()])
    try:
        asyncio.run(to(system, END))
        assert len(system.tap("a.I2C1", I2CTap).phases()) == len(WRITE_THEN_READ)
    finally:
        system.close()


def test_a_bus_run_takes_no_gil_and_a_tap_read_runs_no_user_code(pair: Build) -> None:
    system = pair()
    a, b = system.members["a"], system.members["b"]
    handles = [m._handle for m in (a, b)]  # pyright: ignore[reportPrivateUsage]
    before = [handle.gil_entries() for handle in handles]
    asyncio.run(to(system, END))
    system.tap("a.I2C1", I2CTap).phases()
    gc.collect()
    assert [handle.gil_entries() for handle in handles] == before
