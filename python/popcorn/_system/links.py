from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal, NamedTuple, cast

from popcorn._errors import (
    ConfigError,
)
from popcorn._machine_description import (
    _FrozenMap,  # pyright: ignore[reportPrivateUsage]
)
from popcorn._system.checks import checked_chip_select
from popcorn.units import VirtualDuration

if TYPE_CHECKING:
    pass

STOP_BITS = {0.5: 1, 1.0: 2, 1.5: 3, 2.0: 4}
PARITY = {"none": 0, "even": 1, "odd": 2}


type LineState = Literal["idle", "break"]
"""The state a cut line is held in: `"idle"` keeps it high, `"break"` holds it low."""


@dataclass(frozen=True)
class UartLink:
    """A UART link of a `System`, both directions, between two members or between two
    UARTs of one member.

    A frame whose stop bit ends at the sender's system time t ends at the receiver at exactly
    t + `latency`, whatever the slicing, the thread timing or the system's quantum.

    Attributes:
        a: One end, `"<member>.<UART>"`, such as `"controller.UART4"`.
        b: The other end.
        latency: The line's latency L, above 0. No default: the system's quantum is the
            smallest L, so L is part of every link's declaration.
        baud: The line's declared baud: the shape of injected frames, and the baud a sender
            whose frames differ by more than 3 percent is warned about.
        data_bits: Declared data bits, 7, 8 or 9, at most 8 with a parity bit: a frame
            carries data bits plus the parity bit in at most 9 bits.
        stop_bits: Declared stop bits, 0.5, 1, 1.5 or 2.
        parity: Declared parity, "none", "even" or "odd": the parity bit of injected frames.
            A sender whose USART frames with another parity, CR1.PCE and PS, is warned about
            as for another baud. Each USART computes the parity bit of the frames it sends and
            checks the bit of the frames it receives, a mismatch setting ISR.PE, so a link
            whose ends match its declared parity carries correct bits.
    """

    a: str
    b: str
    latency: VirtualDuration
    baud: int
    data_bits: int = 8
    stop_bits: float = 1.0
    parity: Literal["none", "even", "odd"] = "none"


@dataclass(frozen=True)
class SpiBus:
    """An SPI bus of a `System`: one master SPI and one slave SPI per slave, on other members.

    The master's SPI model runs as with a board device per slave, each selected by a chip
    select pin of the master or by its SS output. A frame the master finishes at system time t
    is clocked by the slave over [t + `latency`, t + `latency` + one frame time], and the slave's
    reply completes the master's frame at t + 2 * `latency` + one frame time, whatever the
    slicing, the thread timing or the system's quantum. SCK pauses for 2 * `latency` between
    frames. A chip select edge reaches the slave `latency` after the master drove it. A frame
    the slave never clocks, dropped by a reset of the slave or cut by a deselect, completes
    with all ones. The master keeps its own board devices on the same SPI, the slaves' SPIs take
    no board controller and no board device.

        popcorn.SpiBus(
            master="a.SPI1",
            slaves={"a.PA4": "b.SPI2", "a.NSS": "c.SPI1"},
            latency=us(5),
        )

    Attributes:
        master: The master's SPI, `"<member>.<SPI>"`, such as `"a.SPI1"`.
        slaves: Each slave's SPI by the master's chip select: `"<master member>.<pin>"`, a GPIO
            pin of the master such as `"a.PA4"` selecting the slave while low, or
            `"<master member>.NSS"` for the master's SS output. Slaves are on members other than
            the master's. A master drives at most one slave through its SS output.
        latency: The bus's latency L, above 0. No default: the system's quantum is the smallest
            L, so L is part of every link's declaration.
    """

    master: str
    slaves: Mapping[str, str]
    latency: VirtualDuration

    def __post_init__(self) -> None:
        slaves = cast(object, self.slaves)
        if not isinstance(slaves, Mapping):
            raise ConfigError(
                f"slaves: {slaves!r} is no mapping of a chip select to a slave",
                key="slaves",
                expected="a mapping of a chip select to a slave SPI",
            )
        object.__setattr__(self, "slaves", _FrozenMap(cast(Mapping[str, str], slaves)))


@dataclass(frozen=True)
class I2CBus:
    """An I2C bus of a `System`: one controller I2C and one I2C per target, on other members.

    The controller's I2C model runs as with a board target per remote target, which takes every
    address: the address match is decided at the target. An address, write or read phase the
    controller ends at system time t is replayed by each target's I2C over [t + `latency`,
    t + `latency` + p], p the phase as the controller clocked it, ten SCL periods for an address
    and nine for a byte. The target's answer is stamped t + `latency` + p + s, s its clock
    stretch, and the controller's phase completes `latency` after the last answer, whatever the
    slicing, the thread timing or the system's quantum. SCL stays low for 2 * `latency` + p + s
    per phase. An address or a written byte is acknowledged when any target or board target
    acknowledges it. The controller acknowledges a read byte. A write or read phase reaches only
    the targets that acknowledged the transfer's last address. A STOP reaches the targets
    `latency` after the controller drove it. A phase a target never answers, dropped by a reset
    of the target or cut by a STOP, gets NACK for an address or a written byte and 0xFF for a
    read byte. The controller keeps its own board targets on the same I2C, the targets' I2Cs take
    no board controller and no board target.

        popcorn.I2CBus(ports=["a.I2C1", "b.I2C1", "c.I2C2"], latency=us(5))

    Attributes:
        ports: The I2Cs, `"<member>.<I2C>"`, the controller's first, then each target's, on
            members other than the controller's, at least one target.
        latency: The bus's latency L, above 0. No default: the system's quantum is the smallest
            L, so L is part of every link's declaration.
    """

    ports: Sequence[str]
    latency: VirtualDuration

    def __post_init__(self) -> None:
        ports = cast(object, self.ports)
        if isinstance(ports, str | bytes) or not isinstance(ports, Sequence):
            raise ConfigError(
                f"ports: {ports!r} is no sequence of I2Cs",
                key="ports",
                expected="a sequence of '<member>.<I2C>', the controller's first",
            )
        object.__setattr__(self, "ports", tuple(cast(Sequence[str], ports)))


type Medium = UartLink | SpiBus | I2CBus
_MEDIA: tuple[type[Medium], ...] = (UartLink, SpiBus, I2CBus)


def named_kind(kind: type) -> str:
    """`kind`'s name with its article, such as "a UartLink", "an SpiBus" or "an I2CBus"."""
    return f"{'an' if kind.__name__.startswith(('Spi', 'I2C')) else 'a'} {kind.__name__}"


def _joined(names: list[str]) -> str:
    return ", ".join(names[:-1]) + " or " + names[-1]


def medium_of(index: int, link: object) -> Medium:
    """Return `links[index]` as a medium, or raise ConfigError at `links[index]`."""
    if not isinstance(link, _MEDIA):
        raise ConfigError(
            f"links[{index}]: a {type(link).__qualname__}, not "
            + _joined([named_kind(kind) for kind in _MEDIA]),
            key=f"links[{index}]",
            expected=_joined([f"a popcorn.{kind.__name__}" for kind in _MEDIA]),
        )
    return link


def check_medium(index: int, link: Medium) -> list[tuple[str, object, str]]:
    """Check the values of `links[index]`, return its ends as (key, value, unit kind)."""
    match link:
        case UartLink():
            check_line(index, link)
            return [(f"links[{index}].{side}", getattr(link, side), "UART") for side in ("a", "b")]
        case SpiBus():
            return check_bus(index, link).ends
        case I2CBus():
            return check_i2c(index, link)


def check_line(index: int, link: UartLink) -> int:
    """Check the line values of `links[index]`, return its word bits, parity bit included."""
    latency = cast(object, link.latency)
    if not isinstance(latency, VirtualDuration) or latency.ps <= 0:
        raise ConfigError(
            f"links[{index}].latency: {link.latency!r} is no duration above 0",
            key=f"links[{index}].latency",
            expected="a VirtualDuration above 0",
        )
    baud, data_bits, parity, stop_bits = cast(
        tuple[object, object, object, object],
        (link.baud, link.data_bits, link.parity, link.stop_bits),
    )
    integral = isinstance(data_bits, int) and not isinstance(data_bits, bool)
    word_bits = cast(int, data_bits) + (0 if parity == "none" else 1) if integral else 0
    for name, refused, expected in (
        (
            "baud",
            not isinstance(baud, int) or isinstance(baud, bool) or baud <= 0,
            "a baud above 0",
        ),
        ("parity", not isinstance(parity, str) or parity not in PARITY, "'none', 'even' or 'odd'"),
        ("data_bits", word_bits not in (7, 8, 9), "7 to 9 data bits, the parity bit included"),
        (
            "stop_bits",
            not isinstance(stop_bits, int | float)
            or isinstance(stop_bits, bool)
            or stop_bits not in STOP_BITS,
            "0.5, 1, 1.5 or 2 stop bits",
        ),
    ):
        if refused:
            key = f"links[{index}].{name}"
            raise ConfigError(
                f"{key}: {getattr(link, name)!r} is no UART line value", key=key, expected=expected
            )
    return word_bits


class _CheckedBus(NamedTuple):
    """What `check_bus` found: the ends as (key, value, unit kind), the master, then each
    slave, and the GPIO port and pin of each chip select."""

    ends: list[tuple[str, object, str]]
    lines: dict[object, tuple[str, int | None]]


def check_bus(index: int, bus: SpiBus) -> _CheckedBus:
    """Check the values of `links[index]`, an `SpiBus`, return its ends and chip selects."""
    latency = cast(object, bus.latency)
    if not isinstance(latency, VirtualDuration) or latency.ps <= 0:
        raise ConfigError(
            f"links[{index}].latency: {bus.latency!r} is no duration above 0",
            key=f"links[{index}].latency",
            expected="a VirtualDuration above 0",
        )
    master = cast(object, bus.master)
    ends: list[tuple[str, object, str]] = [(f"links[{index}].master", master, "SPI")]
    lines: dict[object, tuple[str, int | None]] = {}
    if not isinstance(master, str) or "." not in master:
        return _CheckedBus(ends, lines)
    owner = master.partition(".")[0]
    slaves = cast(object, bus.slaves)
    if not isinstance(slaves, Mapping) or not slaves:
        raise ConfigError(
            f"links[{index}].slaves: {slaves!r} is no mapping of a chip select to a slave",
            key=f"links[{index}].slaves",
            expected="a mapping of a chip select to a slave SPI, at least one",
        )
    for select, slave in cast(Mapping[object, object], slaves).items():
        key = f"links[{index}].slaves[{select!r}]"
        line = checked_chip_select(owner, select)
        if line is None:
            raise ConfigError(
                f"{key}: {select!r} is no chip select of {owner or 'the master'}",
                key=key,
                expected="'<master member>.<pin>', such as 'a.PA4', or '<master member>.NSS'",
            )
        if line in lines.values():
            raise ConfigError(
                f"{key}: {select!r} names a chip select another slave of the bus uses",
                key=key,
                expected="a chip select no other slave of the bus uses",
            )
        lines[select] = line
        if isinstance(slave, str) and slave.partition(".")[0] == owner:
            raise ConfigError(
                f"{key}: {slave} is on the master's member",
                key=key,
                expected="an SPI of another member",
            )
        ends.append((key, slave, "SPI"))
    return _CheckedBus(ends, lines)


def check_i2c(index: int, bus: I2CBus) -> list[tuple[str, object, str]]:
    """Check the values of `links[index]`, an `I2CBus`, return its ends as (key, value, unit
    kind), the controller first."""
    latency = cast(object, bus.latency)
    if not isinstance(latency, VirtualDuration) or latency.ps <= 0:
        raise ConfigError(
            f"links[{index}].latency: {bus.latency!r} is no duration above 0",
            key=f"links[{index}].latency",
            expected="a VirtualDuration above 0",
        )
    ports = cast(Sequence[object], bus.ports)
    if len(ports) < 2:
        raise ConfigError(
            f"links[{index}].ports: {ports!r} holds no target",
            key=f"links[{index}].ports",
            expected="the controller's I2C, then at least one target's",
        )
    ends = [(f"links[{index}].ports[{at}]", port, "I2C") for at, port in enumerate(ports)]
    controller = ports[0]
    if isinstance(controller, str) and "." in controller:
        owner = controller.partition(".")[0]
        for key, port, _ in ends[1:]:
            if isinstance(port, str) and port.partition(".")[0] == owner:
                raise ConfigError(
                    f"{key}: {port} is on the controller's member",
                    key=key,
                    expected="an I2C of another member",
                )
    return ends
