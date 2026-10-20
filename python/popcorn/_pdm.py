"""PDM microphones and digital filter outputs of the MDF and ADF in virtual time."""

from __future__ import annotations

from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal, cast

from popcorn import _core
from popcorn._errors import ConfigError, ModelError
from popcorn._streams import Poster, Waiter, check_wait, read_tagged, written
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._refs import Pending

type Edge = Literal["rising", "falling"]
"""The clock edge on which a sensor drives a serial interface data line."""

EDGES: tuple[Edge, ...] = ("rising", "falling")


@dataclass(frozen=True, slots=True)
class FilterSample:
    """One sample a digital filter wrote to its RXFIFO, or tried to.

    Attributes:
        pcm: Signed 24-bit value, PCM[23:0] as DFLTxDR bits 31:8 hold it.
        at: Virtual time of the write in `m.time`'s base: the clock edge of the bit that
            completed it. A sample kept across a reset carries its time from before the reset.
        overrun: The RXFIFO was full: DOVRF set and the sample dropped. The model's RXFIFO has
            4 entries and no DFLTxDR stage behind them, RM0456 39.4.13, so it overruns one
            sample earlier than silicon, which holds a fifth sample in DFLTxDR.
        captured: False when the ADF's sound activity detector kept the sample out of the
            RXFIFO, SADCR.DATCAP. Always True for the MDF.
    """

    pcm: int
    at: VirtualTime
    overrun: bool
    captured: bool


class PdmMic:
    """The sensor on one clock edge of one serial interface data line,
    `m.periph.MDF1.mic(0, "falling")`.

    Each serial interface carries two sensors, one per clock edge, the bsx_r and bsx_f streams
    of RM0456 39.4.6. A sensor sends its queued input one bit per edge of its kind, only while
    the interface clocks bits in: SITFEN, the CCK it selects and the kernel clock all running.
    With nothing queued it sends silence, the modulator held at 0, and counts `idle_bits`.

    `write_pcm` queues 16-bit PCM that a popcorn-defined 2nd-order single-bit sigma-delta
    modulator turns into the bitstream, as a PDM microphone would. `write_pdm` queues raw bits.
    Writes queue behind each other, the modulator runs on across PCM writes and idle bits.

    Every sensor of a built-in MDF or ADF is connected when the machine is built. The input
    lives outside the machine: a reset of any kind keeps it, `Machine.restore` drops it, resets
    the modulator and clears `idle_bits`.

    The model pulls bits lazily: at each sample a filter completes, at each register access
    whose result depends on them and at each write. `queued_samples`, `queued_bits` and
    `idle_bits` count what it pulled so far, so with no filter running they can lag the clock.
    A reset stops the clock: the bits clocked up to its time are taken, the rest stay queued
    and are sent once the firmware enables the interface again. `Machine.snapshot` also takes
    the bits clocked up to its time first, so the snapshot holds them and the counters count
    them, and a restore never sends them again.

    With RFOVRF set the model keeps delivering exact samples, where silicon gives invalid ones,
    RM0456 39.4.8.
    """

    __slots__ = ("_link", "_post", "edge", "name", "sitf")

    def __init__(self, name: str, sitf: int, edge: Edge, link: _core.PdmLink, post: Poster) -> None:
        """Wrap a native link. Use `m.periph.<MDF|ADF>.mic(sitf, edge)` instead."""
        self.name = name
        self.sitf = sitf
        self.edge: Edge = edge
        self._link = link
        self._post = post

    @property
    def queued_samples(self) -> int:
        """PCM samples written and not reached yet by the model's last pull, the one being
        sent included. See the class doc on the lag."""
        return self._link.queued_samples()

    @property
    def queued_bits(self) -> int:
        """Raw bits written and not pulled by the model yet. See the class doc on the lag."""
        return self._link.queued_bits()

    @property
    def idle_bits(self) -> int:
        """Bits of silence the model pulled from an empty queue, since power-on or the last
        `Machine.restore`. See the class doc on the lag."""
        return self._link.idle_bits()

    def write_pcm(
        self, samples: Sequence[int], *, hz: int, at: VirtualTime | None = None
    ) -> Pending[VirtualTime]:
        """Queue 16-bit PCM samples at `hz`, -32768 to 32767 for full scale 1.0. Posted at the
        call as one command, as `UartStream.write`: at once while the machine is parked, at the
        next block boundary while it runs, or at the first boundary at or after `at`. Await the
        result for the virtual time they were queued at. Edges before that time send the input
        queued before.

        Bit k of the write carries sample `floor(k x hz / bit_hz)`, `bit_hz` the rate of the
        interface's clock edges of this kind, exact integer arithmetic. The modulator
        overloads above about 0.9 full scale, as a microphone clips.

        Raises:
            TypeError: `hz` or a sample is a bool or no int.
            ValueError: A sample is outside -32768 to 32767, `hz` outside 1 to 2**32 - 1, or
                `at` as for `UartStream.write`.
            RuntimeError: Called from a handler or a model.
        """
        _check_int(hz, "hz")
        if not 1 <= hz < 1 << 32:
            raise ValueError(f"a sample rate is 1 to 2**32 - 1 Hz, not {hz!r}")
        values = list(samples)
        for sample in values:
            _check_int(sample, "a PCM sample")
            if not -(1 << 15) <= sample < 1 << 15:
                raise ValueError(f"a PCM sample is -32768 to 32767, not {sample!r}")
        return written(self._post(self._link.command_write_pcm(values, hz), at))

    def write_pdm(
        self,
        data: bytes | bytearray | memoryview,
        *,
        bits: int | None = None,
        at: VirtualTime | None = None,
    ) -> Pending[VirtualTime]:
        """Queue raw bits, one per clock edge of this kind, the MSB of `data[0]` first, 1 for a
        high data line. Posted as `write_pcm` is.

        Args:
            data: Packed bits.
            bits: Bits taken from `data`, `8 x len(data)` by default.

        Raises:
            TypeError: `data` is not `bytes`, `bytearray` or `memoryview`, or `bits` is a bool
                or no int.
            ValueError: `bits` is negative or above `8 x len(data)`, or `at` as for
                `UartStream.write`.
            RuntimeError: Called from a handler or a model.
        """
        raw = cast(object, data)
        if not isinstance(raw, bytes | bytearray | memoryview):
            raise TypeError(f"data is bytes, not {type(raw).__name__}")
        packed = bytes(cast(bytes, raw))
        count = 8 * len(packed) if bits is None else bits
        _check_int(count, "bits")
        if not 0 <= count <= 8 * len(packed):
            raise ValueError(f"bits is 0 to {8 * len(packed)} for {len(packed)} bytes, not {count}")
        return written(self._post(self._link.command_write_pdm(packed, count), at))

    def __repr__(self) -> str:
        return f"PdmMic({self.name}.SITF{self.sitf}.{self.edge})"


class FilterTap:
    """The samples one digital filter writes to its RXFIFO, `m.periph.MDF1.output(0)`.

    Capture starts at power-on or at the last `Machine.restore`: every filter of a built-in MDF
    or ADF is connected when the machine is built. Samples are kept natively, 16 bytes each, up
    to 262 144 per filter, 16.4 s at 16 kHz. A full queue drops its oldest sample and counts it
    in `dropped`. The STM32U575 has 7 filters: left unread, they hold at most 28 MiB.

    Every sample the filter completes is kept, those that found the RXFIFO full with `overrun`
    set, and those the ADF's sound activity detector kept out with `captured` False. A reset
    keeps them: a sample from before it keeps its `at` in the old base, and every read takes it
    whatever its time or deadline. `Machine.restore` drops the samples not read yet and resets
    `dropped`.

    The model's RXFIFO overruns one sample earlier than silicon, see `FilterSample.overrun`.
    With RFOVRF set the model keeps delivering exact samples, where silicon gives invalid ones,
    RM0456 39.4.8.
    """

    __slots__ = ("_link", "_wait", "filter", "name")

    def __init__(self, name: str, filter: int, link: _core.PdmTapLink, wait: Waiter) -> None:
        """Wrap a native link. Use `m.periph.<MDF|ADF>.output(filter)` instead."""
        self.name = name
        self.filter = filter
        self._link = link
        self._wait = wait

    @property
    def available(self) -> int:
        """Samples the filter wrote, not read yet."""
        return self._link.available()

    @property
    def dropped(self) -> int:
        """Samples dropped because the queue held its limit, since power-on or the last
        `Machine.restore`."""
        return self._link.dropped()

    async def read(
        self,
        n: int,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> list[FilterSample]:
        """Run the machine until `n` samples arrived, or `for_` elapsed, and take them.

        Inside `running()`, this awaits the background run instead of driving the machine.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`, and caps
        what the read takes: no sample of the current reset written past it, even one queued
        at the call. Already past, the read ends at once with the samples written by it.

        A read that waits takes only the samples written by the time its wait stopped, since
        the last reset before that stop, in one native step: later samples stay queued for the
        next read, even those a free run wrote before this read resumed, across a reset too.
        Samples kept from before that reset are taken whatever their time. So with `deadline`
        or neither, the samples depend only on the input and the firmware, the same parked and
        running. Inside a free run `for_` counts from the boundary where the wait registered,
        which wall time sets.

        Returns:
            Up to `n` samples, fewer when `for_` elapsed, `deadline` passed or the core went
                idle first.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            ValueError: `n` is negative.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
        """
        check_wait(for_, deadline)
        if n < 0:
            raise ValueError(f"cannot read {n} samples")
        return _samples(await read_tagged(self._link, n, self._wait, for_, deadline))

    def take(self) -> list[FilterSample]:
        """Every sample the filter wrote and not read yet, at once, never running the machine.
        Allowed parked and running, from any thread. A sample arriving during the call stays
        queued for the next one. A take while a `read` of the same tap waits can leave that
        read short."""
        link = self._link
        return _samples(link.drain(link.available()))

    def __repr__(self) -> str:
        return f"FilterTap({self.name}.DFLT{self.filter})"


def _check_int(value: object, what: str) -> None:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{what} is an int, not {type(value).__name__}")


def _samples(records: Iterable[_core.PdmSampleRecord]) -> list[FilterSample]:
    return [
        FilterSample(record.pcm, VirtualTime(record.at_ps), record.overrun, record.captured)
        for record in records
    ]


class PdmPorts:
    """The Python ends of a machine's MDF and ADF instances, a link per sensor and per filter."""

    __slots__ = ("_aliases", "_mics", "_post", "_sitfs", "_streams", "_taps", "_wait")

    def __init__(
        self,
        connect: Callable[[str, int, str], _core.PdmLink],
        tap: Callable[[str, int], _core.PdmTapLink],
        units: Mapping[str, int],
        aliases: Mapping[str, str],
        wait: Waiter,
        post: Poster,
    ) -> None:
        """Connect every sensor and filter of every instance in `units`, which maps its name to
        its serial interface count, so silence flows and samples are kept from power-on."""
        self._aliases = dict(aliases)
        self._wait = wait
        self._post = post
        self._sitfs = dict(units)
        self._mics: dict[tuple[str, int, Edge], _core.PdmLink] = {}
        self._taps: dict[tuple[str, int], _core.PdmTapLink] = {}
        for name, count in units.items():
            for index in range(count):
                for edge in EDGES:
                    self._mics[name, index, edge] = connect(name, index, edge)
                self._taps[name, index] = tap(name, index)
        self._streams: dict[tuple[str, int, str], PdmMic | FilterTap] = {}

    def _serving(self, name: str, what: str) -> tuple[str, int]:
        name = self._aliases.get(name, name)
        count = self._sitfs.get(name)
        if count is None:
            raise ModelError(
                f"{name} has no built-in MDF or ADF model, so no {what}", peripheral=name
            )
        return name, count

    def _index(self, name: str, what: str, index: object, count: int) -> int:
        if isinstance(index, bool) or not isinstance(index, int) or not 0 <= index < count:
            raise ConfigError(
                f"{name}.{what}: an index from 0 to {count - 1}, not {index!r}",
                key=f"{name}.{what}",
                expected=f"an index from 0 to {count - 1}",
            )
        return index

    def mic(self, name: str, sitf: int, edge: str) -> PdmMic:
        """The sensor on clock edge `edge` of serial interface `sitf` of instance `name`.

        Raises:
            ModelError: The peripheral's model is not the built-in MDF or ADF.
            ConfigError: `sitf` is past the last interface, or `edge` is neither "rising" nor
                "falling".
        """
        name, count = self._serving(name, "microphone")
        sitf = self._index(name, "mic", sitf, count)
        if edge not in EDGES:
            raise ConfigError(
                f"{name}.mic: a clock edge rising or falling, not {edge!r}",
                key=f"{name}.mic",
                expected="a clock edge rising or falling",
            )
        chosen: Edge = "rising" if edge == "rising" else "falling"
        key = (name, sitf, chosen)
        mic = self._streams.get(key)
        if not isinstance(mic, PdmMic):
            mic = PdmMic(name, sitf, chosen, self._mics[name, sitf, chosen], self._post)
            self._streams[key] = mic
        return mic

    def output(self, name: str, filter: int) -> FilterTap:
        """The samples digital filter `filter` of instance `name` writes.

        Raises:
            ModelError: The peripheral's model is not the built-in MDF or ADF.
            ConfigError: `filter` is past the last filter.
        """
        name, count = self._serving(name, "filter output")
        filter = self._index(name, "output", filter, count)
        key = (name, filter, "")
        tap = self._streams.get(key)
        if not isinstance(tap, FilterTap):
            tap = FilterTap(name, filter, self._taps[name, filter], self._wait)
            self._streams[key] = tap
        return tap

    def discard(self) -> None:
        """Drop queued input, reset the modulators and idle counts, drop unread samples and the
        drop counts, at a restore."""
        for link in self._mics.values():
            link.discard()
        for tap in self._taps.values():
            tap.discard()
