"""SAI audio streams in virtual time: frames a block transmits, words a block receives."""

from __future__ import annotations

from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal

from popcorn import _core
from popcorn._errors import ConfigError, ModelError
from popcorn._streams import Poster, Waiter, check_wait, cutoff, written
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._refs import Pending

type Block = Literal["A", "B"]
"""An audio block of an SAI."""

BLOCKS: tuple[Block, ...] = ("A", "B")


@dataclass(frozen=True, slots=True)
class AudioLayout:
    """Where the slots of an audio frame sit on the bus, as the block was configured for it.

    Positions count bit clocks from the frame sync edge, RM0456 69.4.6 and 69.4.7. A slot's
    data occupies bits `first_bit_offset` to `first_bit_offset + data_bits - 1` of the slot,
    zeros around it on transmit. A firmware with the wrong FSDEF, FSOFF or slot order sends the
    same slot words as a right one, the layout tells them apart. A transmitted frame carries
    its layout in `AudioFrame.layout`. A receiving block's layout is
    `AudioStream.receive_layout`.

    Attributes:
        frame_bits: Bit clocks per frame, FRL + 1.
        slot_bits: Bits per slot, SLOTSZ resolved: the data size, 16 or 32.
        first_bit_offset: First data bit within a slot, FBOFF.
        fs_definition: Frame sync marks the start of frame and the channel side, FSDEF: the
            second half of the slots starts at half the frame.
        fs_offset: Frame sync asserted one bit clock before slot 0, FSOFF: every slot starts
            one bit clock later.
        fs_active_high: Frame sync active high, FSPOL.
        fs_active_bits: Frame sync active level length in bit clocks, FSALL + 1.
        slot_starts: Start of each slot, NBSLOT + 1 of them, FSOFF included. A start at or
            past `frame_bits` is a slot that does not fit in the frame and carries no data.
    """

    frame_bits: int
    slot_bits: int
    first_bit_offset: int
    fs_definition: bool
    fs_offset: bool
    fs_active_high: bool
    fs_active_bits: int
    slot_starts: tuple[int, ...]


@dataclass(frozen=True, slots=True)
class AudioFrame:
    """One frame an SAI block transmitted.

    The layout is kept per frame because firmware may reconfigure the block between two
    frames, and a reader across that change needs each frame's own layout.

    Attributes:
        slots: One word per slot, NBSLOT + 1 of them, data right-aligned. Inactive slots and
            slots sent while the FIFO underran are 0.
        data_bits: Data size in bits, 8 to 32.
        active_slots: Active slots, bit n for slot n.
        mono: Mono mode with two slots.
        at: Virtual time the frame started. A UART frame carries its end, at its stop bit. An
            audio frame is defined by its frame sync edge, so it carries its start.
        layout: Where the slots sit in the frame, in bit clocks from the frame sync edge.
    """

    slots: tuple[int, ...]
    data_bits: int
    active_slots: int
    mono: bool
    at: VirtualTime
    layout: AudioLayout


@dataclass(frozen=True, slots=True)
class AudioOptions:
    """What the board wires to one SAI audio block, besides the Python stream.

    Attributes:
        decimation: Frame time multiplier, at least 1, 1 for the configured rate.
        external_frame_hz: Frame rate in Hz of the external clock master of a slave block in
            asynchronous mode, 0 for none. A clock already running at power-on: its phase is
            taken at the block's first enable.
        clocked_by: The bus device driving the block's external clock through
            `DeviceBoard.set_audio_clock`, as `<I2C>:<address>` such as "I2C1:0x38" or
            `<SPI>:<chip select>` such as "SPI1:PA4", "SPI1:SS" for the hardware NSS. A bus
            alias, such as "SEC_I2C1", names the bus serving it. None for none: no device may
            drive it. A wired block enabled while its clock is stopped waits without the
            warning an unclocked slave block logs.
    """

    decimation: int = 1
    external_frame_hz: int = 0
    clocked_by: str | None = None


class AudioStream:
    """The far end of an SAI audio block, `m.periph.SAI1.audio("A")`.

    Capture starts at power-on or at the last `Machine.restore`: every block of a built-in
    SAI is connected when the machine is built. Frames are kept natively, 8 bytes plus 4 per
    slot each, 16 bytes for 16-bit stereo, up to 262 144 frames per block, 5.4 s at 48 kHz.
    A full queue drops its oldest frame and counts it in `dropped`. The STM32U575 has four
    blocks: left unread, they hold at most 16 MiB of stereo, 72 MiB of 16-slot TDM. A reset
    keeps the frames: a frame from before it keeps its `at` in the old base, and every read
    takes it whatever its time or deadline.

    `Machine.restore` drops the frames not read yet, resets `dropped` and clears
    `receive_layout`. It also drops the words written and not served yet. Unlike the bytes
    written to a UART, which are UART model state and rewind with a snapshot, they live outside
    the machine, so after a restore the receiver sees 0 until the test writes again.
    """

    __slots__ = ("_link", "_post", "_wait", "block", "name")

    def __init__(
        self,
        name: str,
        block: Block,
        link: _core.AudioLink,
        wait: Waiter,
        post: Poster,
    ) -> None:
        """Wrap a native link. Use `m.periph.<SAI>.audio(block)` instead."""
        self.name = name
        self.block = block
        self._link = link
        self._wait = wait
        self._post = post

    @property
    def available(self) -> int:
        """Frames the block transmitted, not read yet."""
        return self._link.available()

    @property
    def dropped(self) -> int:
        """Transmitted frames dropped because the queue held its limit, since power-on or the
        last `Machine.restore`."""
        return self._link.dropped()

    @property
    def queued(self) -> int:
        """Words written and not served to the receiving block yet."""
        return self._link.queued()

    @property
    def receive_layout(self) -> AudioLayout | None:
        """Layout of the last frame the receiving block took, as the block was configured for
        it, None before the first one since power-on or the last `Machine.restore`.

        The block takes a frame when its first active slot ends, at the latest at the frame
        end, so this is the layout the firmware's receiver runs with: an FSDEF, FSOFF or slot
        order that does not match the words written shows here. A synchronous follower reports
        the frame length of the block it follows. A reset keeps it: the next frame the block
        takes replaces it. Allowed parked and running, from any thread, as `available`. While
        the machine runs it is the layout of the latest frame taken by then: read it from
        `m.sample` or once the run stopped for a value that depends only on virtual time.
        """
        record = self._link.receive_layout()
        return None if record is None else _layout_of(record)

    @property
    def external_frame_hz(self) -> int:
        """Frame rate of the external clock master of this slave block, 0 when stopped.

        Setting it starts, changes or stops the clock, as the board drives it. A running clock
        changes rate or stops at the end of its frame in progress, so a stop and a start within
        one frame keep its phase. A stopped clock starts at once: an enabled asynchronous slave
        block starts its first frame now, one enabled later at the clock's next frame boundary.
        Board state: `Machine.restore` restores it, a reset keeps it in phase, except POWER_ON
        and BROWN_OUT, which power the codec down and return it to `AudioOptions`.

        Raises:
            MachineRunning: Read or set while the machine is busy, outside its callbacks: use
                `m.ref.periph.<SAI>.audio(block).external_frame_hz`.
            ValueError: Set to a value outside 0 to 2**32 - 1.
        """
        return self._link.external_frame_hz()

    @external_frame_hz.setter
    def external_frame_hz(self, hz: int) -> None:
        if not _is_frame_rate(hz):
            raise ValueError(f"an external frame rate is 0 to 2**32 - 1 Hz, not {hz!r}")
        self._link.set_external_frame_hz(hz)

    async def read_frames(
        self,
        n: int,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> list[AudioFrame]:
        """Run the machine until `n` frames arrived, or `for_` elapsed, and take them.

        Inside `running()`, this awaits the background run instead of driving the machine.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`, and caps
        what the read takes: no frame that ended past it, even one queued at the call. Already
        past, the read ends at once with the frames that ended by it.

        A read that waits takes only the frames that ended by the time its wait stopped, since
        the last reset before that stop, in one native step: later frames stay queued for the
        next read, even those a free run sent before this read resumed, across a reset too.
        Frames kept from before that reset are taken whatever their time. So with `deadline`
        or neither, the frames depend only on what the block sent, the same parked and running.
        Inside a free run `for_` counts from the boundary where the wait registered, which wall
        time sets. A frame's `at` is its start, one frame time before its end.

        Returns:
            Up to `n` frames, fewer when `for_` elapsed, `deadline` passed or the core went
                idle first.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            ValueError: `n` is negative.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
        """
        check_wait(for_, deadline)
        if n < 0:
            raise ValueError(f"cannot read {n} frames")
        if self._link.available() >= n:
            return _audio_frames(self._link.drain_until(n, cutoff(None, deadline), None))
        stop = await self._wait(self._link.predicate_available(n), for_, deadline)
        return _audio_frames(self._link.drain_until(n, cutoff(stop, deadline), stop.resets))

    def take_frames(self) -> list[AudioFrame]:
        """Every frame the block transmitted and not read yet, at once, never running the
        machine. Allowed parked and running, from any thread. A frame arriving during the call
        stays queued for the next one. A take while a `read_frames` of the same stream waits can
        leave that read short."""
        link = self._link
        return _audio_frames(link.drain(link.available()))

    def write(self, words: Sequence[int], *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Queue words for a receiving block: one per slot in slot order, inactive slots
        included. Once they are served the block receives 0. Posted at the call as one command,
        at the next block boundary or the first one at or after `at`, as `UartStream.write`:
        await the result for the time they were queued at.

        Raises:
            RuntimeError: Called from a handler or a model.
            ValueError: A word is outside 0 to 2**32 - 1, or `at` as for `UartStream.write`.
        """
        values = list(words)
        for word in values:
            if isinstance(word, bool) or not 0 <= word < 1 << 32:
                raise ValueError(f"an audio word is 0 to 2**32 - 1, not {word!r}")
        link = self._link
        return written(self._post(link.command_write(values), at))

    def __repr__(self) -> str:
        return f"AudioStream({self.name}.{self.block})"


def _audio_frames(records: Iterable[_core.AudioRecord]) -> list[AudioFrame]:
    layouts: dict[AudioLayout, AudioLayout] = {}
    frames: list[AudioFrame] = []
    for record in records:
        layout = _layout_of(record)
        frames.append(
            AudioFrame(
                tuple(record.slots),
                record.data_bits,
                record.active_slots,
                record.mono,
                VirtualTime(record.at_ps),
                layouts.setdefault(layout, layout),
            )
        )
    return frames


def _layout_of(record: _core.AudioRecord) -> AudioLayout:
    return AudioLayout(
        record.frame_bits,
        record.slot_bits,
        record.first_bit_offset,
        record.fs_definition,
        record.fs_offset,
        record.fs_active_high,
        record.fs_active_bits,
        tuple(record.slot_starts),
    )


def _is_frame_rate(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value < 1 << 32


class AudioPorts:
    """The Python ends of a machine's SAI blocks, one link and one stream per block."""

    __slots__ = ("_aliases", "_links", "_post", "_streams", "_wait")

    def __init__(
        self,
        connect: Callable[[str, str, int, int, bool], _core.AudioLink],
        sais: Iterable[str],
        aliases: Mapping[str, str],
        options: Mapping[str, Mapping[Block, AudioOptions]],
        wait: Waiter,
        post: Poster,
    ) -> None:
        """Connect both blocks of every SAI in `sais` through `connect` at once, with their
        `options`, so each one keeps what the firmware transmits from power-on."""
        self._aliases = dict(aliases)
        self._wait = wait
        self._post = post
        self._links: dict[tuple[str, Block], _core.AudioLink] = {}
        for name in sais:
            for block in BLOCKS:
                chosen = options.get(name, {}).get(block, AudioOptions())
                self._links[name, block] = connect(
                    name,
                    block,
                    chosen.decimation,
                    chosen.external_frame_hz,
                    chosen.clocked_by is not None,
                )
        self._streams: dict[tuple[str, Block], AudioStream] = {}

    def serving(self, name: str) -> str:
        """The SAI that serves `name`, `name` itself unless it is an alias."""
        return self._aliases.get(name, name)

    def stream(self, name: str, block: str) -> AudioStream:
        """The stream of audio block `block` of SAI `name`.

        Raises:
            ConfigError: `block` is neither "A" nor "B".
            ModelError: The peripheral's model is not the built-in SAI.
        """
        if block not in BLOCKS:
            raise ConfigError(
                f"{name}.audio: an audio block A or B, not {block!r}",
                key=f"{name}.audio",
                expected="an audio block A or B",
            )
        chosen: Block = "A" if block == "A" else "B"
        name = self.serving(name)
        key = (name, chosen)
        stream = self._streams.get(key)
        if stream is None:
            link = self._links.get(key)
            if link is None:
                raise ModelError(
                    f"{name} has no built-in SAI model, so no audio stream", peripheral=name
                )
            stream = self._streams[key] = AudioStream(name, chosen, link, self._wait, self._post)
        return stream

    def discard(self) -> None:
        """Drop kept frames, unserved words, receive layouts and the drop counts, at a
        restore."""
        for link in self._links.values():
            link.discard()
