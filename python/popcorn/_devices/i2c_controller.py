from __future__ import annotations

import typing
from collections.abc import Sequence
from dataclasses import dataclass
from typing import cast, override

from popcorn import _core
from popcorn._devices.base import ATTACHED
from popcorn._devices.naming import refuse_reserved
from popcorn._enums import I2CError, I2CPhase
from popcorn._errors import ConfigError
from popcorn.units import VirtualDuration, VirtualTime

if typing.TYPE_CHECKING:
    from popcorn._refs import Pending

_MAX_PHASES = 4096
_MAX_I2C_AT_PS = 1 << 63
_MIN_SCL_HZ = 1_000
_MAX_SCL_HZ = 5_000_000
_START, _WRITE, _READ, _STOP = range(4)


@dataclass(frozen=True, slots=True)
class I2CAnswer:
    """One phase an `I2CController` queued, as the MCU answered it.

    Attributes:
        unit: The number the phase was queued with, increasing in queuing order on the I2C.
        phase: The phase, an address, a written byte or a read byte.
        ack: Whether the MCU acknowledged the address or the written byte. For a read byte, the
            acknowledge the controller gave it.
        byte: The byte the MCU sent in a read phase, 0xFF for the other phases.
        error: The error the phase ended with, None for none. None for a phase the MCU answers
            itself.
        at: Machine time since reset at which SCL was released after the phase.
    """

    unit: int
    phase: I2CPhase
    ack: bool
    byte: int
    error: I2CError | None
    at: VirtualTime


class I2CController(_core.I2CControllerBase):
    """A controller on an I2C bus, outside the MCU, addressing an I2C the firmware runs in
    target mode, with OA1EN, OA2EN or GCEN. Written in Python.

    Attach it with `Machine(devices={"I2C1": [I2CController()]})` or a machine description's
    `Periph(devices=[Device(I2CController, scl_hz=...)])`, alone on its I2C: no `I2CTarget` and
    no second controller on that instance.

    Phases: `start`, `write`, `read` and `stop` queue a START with its address byte, written
    bytes, read bytes and a STOP. Each START, write and read phase lasts nine SCL periods, eight
    bits and the acknowledge, and starts at the latest of its `at`, the end of every phase
    queued before it and the time of every STOP queued before it. A target that stretches SCL,
    with ADDR, RXNE or TXIS pending, holds the phase after it and every STOP until it releases
    SCL. A START with no STOP before it is a repeated START.

    STOP: `stop` takes effect at its own time. A STOP due while the target stretches SCL runs
    after the stretched byte's answer. A phase still queued or running at the STOP otherwise is
    dropped, as on a bus where the controller sends STOP in the middle of a byte, so a script
    queues the STOP at or after the end of the last phase, or after its answer.

    Answers: `on_answer` hears one `I2CAnswer` per START, write and read phase, at the time SCL
    is released after it, on the machine's thread with the GIL. A phase the I2C drops, by a
    reset of the MCU or of the I2C, by a STOP due before its end or with no acknowledged
    address, is answered with the undriven reply, NACK for an address or a written byte, 0xFF
    with ack False for a read byte, at the time the I2C reports it. So every phase gets exactly
    one answer. A STOP gets none.

    Time: `at` is the machine time since reset, as `Machine.time`, not the time since power-on
    an `I2CTarget` hook gets.

    State: the queued phases are the I2C model's state, saved with a snapshot. `answers`
    belongs to the test: a snapshot does not save it and a restore does not change it.

    Lifetime: as `I2CTarget`.

    Names: a name starting with `_` that this class defines is reserved. A subclass defining one
    raises TypeError at class definition, and an instance attribute with one raises TypeError when
    the machine takes the device. Other `_` names are free for the subclass, and an attribute a
    subclass sets never replaces the class's own state, which is name-mangled.
    """

    answers: list[I2CAnswer]
    """The answers heard, in order, as the default `on_answer` appends them."""

    def __init_subclass__(cls, **kwargs: object) -> None:
        super().__init_subclass__(**kwargs)
        refuse_reserved(cls, I2CController)

    def __init__(self, *, scl_hz: int = 100_000) -> None:
        """Clock phases at `scl_hz`.

        Raises:
            TypeError: `scl_hz` is no int.
            ValueError: `scl_hz` is outside 1 kHz to 5 MHz.
        """
        if isinstance(scl_hz, bool) or not isinstance(cast(object, scl_hz), int):
            raise TypeError(f"I2CController takes scl_hz as an int, not {scl_hz!r}")
        if not _MIN_SCL_HZ <= scl_hz <= _MAX_SCL_HZ:
            raise ValueError(f"an I2C clock is 1 kHz to 5 MHz, not {scl_hz} Hz")
        super().__init__()
        self.__scl_hz = scl_hz
        self.answers = []

    @property
    def scl_hz(self) -> int:
        """The SCL frequency in Hz the controller clocks phases at, 1 kHz to 5 MHz. Read-only."""
        return self.__scl_hz

    @property
    def phase_time(self) -> VirtualDuration:
        """The length of one START, write or read phase, `9 * 10**12 // scl_hz` ps."""
        return VirtualDuration(self._phase_ps)

    @property
    def _phase_ps(self) -> int:
        return 9 * 10**12 // self.__scl_hz

    def start(
        self, address: int, *, read: bool = False, at: VirtualTime | None = None
    ) -> Pending[VirtualTime]:
        """Queue a START, or a repeated START when no `stop` came before it, and the address
        byte of `address`, a read transfer when `read`. Posted as one command as
        `UartStream.write` is: at once while the machine is parked, at the next block boundary
        while it runs. `at` is the earliest start, the time the command runs when None, which
        depends on that boundary while running. Await the result for that earliest start.

        Raises:
            TypeError: `address` is no int, `read` no bool, or `at` no `VirtualTime`.
            ValueError: `address` is outside 0 to 0x7F, or `at` outside 0 to 2**63 ps. From the
                await, `at` is already past when the command runs.
            ConfigError: The controller serves no live machine. From the await, the I2C would
                hold more than 4096 queued phases and STOPs.
            RuntimeError: Called from a handler, a hook or a model.
        """
        if isinstance(address, bool) or not isinstance(cast(object, address), int):
            raise TypeError(f"start() takes an address as an int, not {address!r}")
        if not 0 <= address <= 0x7F:
            raise ValueError(f"an I2C address has 7 bits, 0 to 0x7F, not {address:#x}")
        if not isinstance(cast(object, read), bool):
            raise TypeError(f"start() takes read as a bool, not {read!r}")
        return self._queue([_START], [address << 1 | int(read)], at)

    def write(
        self, data: bytes | Sequence[int], *, at: VirtualTime | None = None
    ) -> Pending[VirtualTime]:
        """Queue one write phase per byte of `data`, as `start` posts and places them.

        Raises:
            TypeError: `data` is no bytes or sequence of ints, or `at` no `VirtualTime`.
            ValueError: `data` holds no byte or more than 4096, a byte is outside 0 to 0xFF,
                or `at` is outside 0 to 2**63 ps. From the await, as `start`.
            ConfigError: As `start`.
            RuntimeError: As `start`.
        """
        if isinstance(data, str) or not isinstance(
            cast(object, data), bytes | bytearray | Sequence
        ):
            raise TypeError(f"write() takes bytes or a sequence of ints, not {data!r}")
        if not 1 <= len(data) <= _MAX_PHASES:
            raise ValueError(f"an I2C write has 1 to 4096 bytes, not {len(data)}")
        checked: list[int] = []
        for byte in data:
            if isinstance(byte, bool) or not isinstance(cast(object, byte), int):
                raise TypeError(f"an I2C byte is an int, not {byte!r}")
            if not 0 <= byte <= 0xFF:
                raise ValueError(f"an I2C byte is 0 to 0xFF, not {byte}")
            checked.append(byte)
        return self._queue([_WRITE] * len(checked), checked, at)

    def read(
        self, count: int, *, ack_last: bool = False, at: VirtualTime | None = None
    ) -> Pending[VirtualTime]:
        """Queue `count` read phases, as `start` posts and places them, the controller
        acknowledging every byte but the last, and the last when `ack_last`.

        Raises:
            TypeError: `count` is no int, `ack_last` no bool, or `at` no `VirtualTime`.
            ValueError: `count` is outside 1 to 4096, or `at` outside 0 to 2**63 ps. From the
                await, as `start`.
            ConfigError: As `start`.
            RuntimeError: As `start`.
        """
        if isinstance(count, bool) or not isinstance(cast(object, count), int):
            raise TypeError(f"read() takes a count as an int, not {count!r}")
        if not 1 <= count <= _MAX_PHASES:
            raise ValueError(f"an I2C read has 1 to 4096 bytes, not {count}")
        if not isinstance(cast(object, ack_last), bool):
            raise TypeError(f"read() takes ack_last as a bool, not {ack_last!r}")
        return self._queue([_READ] * count, [1] * (count - 1) + [int(ack_last)], at)

    def stop(self, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Queue a STOP at `at`, the time the command runs when None, posted as `start` posts.
        Every phase queued before it and still queued or running at its time is dropped and
        answered undriven. A STOP due while the target stretches SCL runs after the stretched
        byte's answer. Await the result for the STOP's time.

        Raises:
            TypeError: `at` is no `VirtualTime`.
            ValueError: `at` is outside 0 to 2**63 ps. From the await, as `start`.
            ConfigError: As `start`.
            RuntimeError: As `start`.
        """
        return self._queue([_STOP], [0], at)

    def on_answer(self, answer: I2CAnswer) -> None:
        """One phase was answered, called on the machine's thread with the GIL, once per
        phase. Appends `answer` to `answers` by default."""
        self.answers.append(answer)

    def _queue(
        self, kinds: list[int], values: list[int], at: VirtualTime | None
    ) -> Pending[VirtualTime]:
        if at is not None and not isinstance(cast(object, at), VirtualTime):
            raise TypeError(f"an I2C phase takes at as a VirtualTime or None, not {at!r}")
        if at is not None and not 0 <= at.ps <= _MAX_I2C_AT_PS:
            raise ValueError(f"an I2C phase at {at} lies outside 0 to 2**63 ps")
        held = ATTACHED.get(id(self))
        post = None if held is None else held[4]()
        if held is None or post is None:
            raise ConfigError(
                f"{self!r} serves no live machine",
                key="devices",
                expected="a controller serving a live machine",
            )
        return post(held[5], at, self._phase_ps, kinds, values)

    def _answered(self, unit: int, phase: int, ack: bool, byte: int, error: int, at: int) -> None:
        self.on_answer(
            I2CAnswer(
                unit,
                I2CPhase(phase),
                ack,
                byte,
                I2CError(error) if error else None,
                VirtualTime(at),
            )
        )

    @override
    def __repr__(self) -> str:
        return f"<{type(self).__qualname__} at {self.__scl_hz} Hz>"
