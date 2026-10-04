"""SAI audio streams in virtual time: frames a block transmits, words a block receives."""

from __future__ import annotations

from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass
from typing import Literal

from popcorn import _core
from popcorn._errors import ConfigError, ModelError
from popcorn._streams import Waiter
from popcorn.units import VirtualDuration, VirtualTime

type Block = Literal["A", "B"]
"""An audio block of an SAI."""

BLOCKS: tuple[Block, ...] = ("A", "B")


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
    """

    slots: tuple[int, ...]
    data_bits: int
    active_slots: int
    mono: bool
    at: VirtualTime


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
    blocks: left unread, they hold at most 16 MiB of stereo, 72 MiB of 16-slot TDM.

    `Machine.restore` drops the frames not read yet and resets `dropped`. It also drops the
    words written and not served yet. Unlike the bytes written to a UART, which are UART model
    state and rewind with a snapshot, they live outside the machine, so after a restore the
    receiver sees 0 until the test writes again.
    """

    __slots__ = ("_link", "_stimulus", "_wait", "block", "name")

    def __init__(
        self,
        name: str,
        block: Block,
        link: _core.AudioLink,
        wait: Waiter,
        stimulus: Callable[[], None],
    ) -> None:
        """Wrap a native link. Use `m.periph.<SAI>.audio(block)` instead."""
        self.name = name
        self.block = block
        self._link = link
        self._wait = wait
        self._stimulus = stimulus

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
    def external_frame_hz(self) -> int:
        """Frame rate of the external clock master of this slave block, 0 when stopped.

        Setting it starts, changes or stops the clock, as the board drives it. A running clock
        changes rate or stops at the end of its frame in progress, so a stop and a start within
        one frame keep its phase. A stopped clock starts at once: an enabled asynchronous slave
        block starts its first frame now, one enabled later at the clock's next frame boundary.
        Board state: `Machine.restore` restores it, a reset keeps it in phase, except POWER_ON
        and BROWN_OUT, which power the codec down and return it to `AudioOptions`.

        Raises:
            RuntimeError: Set from another thread while a run is active.
            ValueError: Set to a value outside 0 to 2**32 - 1.
        """
        return self._link.external_frame_hz()

    @external_frame_hz.setter
    def external_frame_hz(self, hz: int) -> None:
        self._stimulus()
        if not _is_frame_rate(hz):
            raise ValueError(f"an external frame rate is 0 to 2**32 - 1 Hz, not {hz!r}")
        self._link.set_external_frame_hz(hz)

    async def read_frames(self, n: int, *, for_: VirtualDuration | None = None) -> list[AudioFrame]:
        """Run the machine until `n` frames arrived, or `for_` elapsed, and take them.

        Inside `running()`, this awaits the background run instead of driving the machine.

        Returns:
            Up to `n` frames, fewer when `for_` elapsed or the core went idle first.

        Raises:
            ValueError: `n` is negative.
            EmulationError: The run stopped for another reason, such as a fault.
        """
        if n < 0:
            raise ValueError(f"cannot read {n} frames")
        if self._link.available() < n:
            await self._wait(lambda: self._link.available() >= n, for_)
        return [
            AudioFrame(
                tuple(record.slots),
                record.data_bits,
                record.active_slots,
                record.mono,
                VirtualTime(record.at_ps),
            )
            for record in self._link.drain(n)
        ]

    async def write(self, words: Sequence[int]) -> None:
        """Queue words for a receiving block: one per slot in slot order, inactive slots
        included. Once they are served the block receives 0.

        Raises:
            RuntimeError: Called from another thread while a run is active.
            ValueError: A word is outside 0 to 2**32 - 1.
        """
        self._stimulus()
        for word in words:
            if isinstance(word, bool) or not 0 <= word < 1 << 32:
                raise ValueError(f"an audio word is 0 to 2**32 - 1, not {word!r}")
        self._link.write(list(words))

    def __repr__(self) -> str:
        return f"AudioStream({self.name}.{self.block})"


def _is_frame_rate(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value < 1 << 32


class AudioPorts:
    """The Python ends of a machine's SAI blocks, one link and one stream per block."""

    __slots__ = ("_aliases", "_links", "_stimulus", "_streams", "_wait")

    def __init__(
        self,
        connect: Callable[[str, str, int, int, bool], _core.AudioLink],
        sais: Iterable[str],
        aliases: Mapping[str, str],
        options: Mapping[str, Mapping[Block, AudioOptions]],
        wait: Waiter,
        stimulus: Callable[[], None],
    ) -> None:
        """Connect both blocks of every SAI in `sais` through `connect` at once, with their
        `options`, so each one keeps what the firmware transmits from power-on."""
        self._aliases = dict(aliases)
        self._wait = wait
        self._stimulus = stimulus
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
            stream = self._streams[key] = AudioStream(
                name, chosen, link, self._wait, self._stimulus
            )
        return stream

    def discard(self) -> None:
        """Drop kept frames, unserved words and the drop counts, at a restore."""
        for link in self._links.values():
            link.discard()
