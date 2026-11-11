from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from typing import TYPE_CHECKING, override

from popcorn import _core
from popcorn._errors import UsbError, UsbTimeout
from popcorn._streams import (
    Poster,
    Waiter,
    check_wait,
    cutoff,
    written,
)
from popcorn._usb.transfer import await_result, raise_for
from popcorn.units import VirtualDuration, VirtualTime

if TYPE_CHECKING:
    from popcorn._refs import Pending

_MUTE = 1
_VOLUME = 2


@dataclass(frozen=True, slots=True)
class UacFormat:
    """The PCM format of an audio stream, from its AS_GENERAL and Type I format descriptors.

    Attributes:
        channels: bNrChannels: samples per frame of audio, interleaved.
        bytes_per_sample: bSubslotSize: bytes one sample takes on the bus, 1 to 4.
        bits: bBitResolution: bits of the subslot that carry the sample.
    """

    channels: int
    bytes_per_sample: int
    bits: int


@dataclass(frozen=True, slots=True)
class UacFrame:
    """One isochronous IN packet of a microphone stream, decoded.

    Attributes:
        samples: Samples, interleaved, each the signed subslot as the bus carried it. Empty for
            a gap: the device answered an empty packet, or none.
        at: Virtual time of its transaction, whether the device answered or not.
    """

    samples: tuple[int, ...]
    at: VirtualTime


@dataclass(frozen=True, slots=True)
class UacFeatureUnit:
    """A feature unit of an audio function, with the values the host last read or set.

    Index 0 of each tuple is the master channel, index n the n-th logical channel.

    Attributes:
        unit: bUnitID.
        channels: Logical channels, the master channel excluded.
        controls: bmaControls per channel: 2 bits per control, bit 0 of a pair readable, bit 1
            writable, mute in bits 1:0, volume in bits 3:2.
        mute: Mute per channel, None where it is not readable or was not read.
        volume: Volume per channel in 1/256 dB, None where it is not readable or was not read.
        volume_range: (minimum, maximum, resolution) of the volume per channel in 1/256 dB, as
            the probe's GET_RANGE gave it, None where not read.
    """

    unit: int
    channels: int
    controls: tuple[int, ...]
    mute: tuple[bool | None, ...]
    volume: tuple[int | None, ...]
    volume_range: tuple[tuple[int, int, int] | None, ...]


class UacStream:
    """One streaming direction of a USB Audio 2.0 function: `audio.speaker` or `audio.mic`.

    The stream is the function's first audio streaming interface in that direction, run at its
    first alternate setting with an isochronous endpoint and a Type I PCM format. `start`
    selects that setting, `stop` goes back to setting 0. The rate is the clock's GET_CUR, so a
    device with a fixed clock runs at its own rate, and the host's frames set it: 1 ms per
    frame, no drift. A real host and device drift apart and resync through feedback or
    adaptive clocking, which the emulation never exercises.

    `interface`, `alternate`, `format`, `sample_rates` and `rate` keep their last values after an
    unplug or a bus reset, until a driver binds again or a restore.
    """

    __slots__ = (
        "__weakref__",
        "_link",
        "_mic",
        "_pending",
        "_post",
        "_wait",
        "function",
        "name",
    )

    def __init__(
        self,
        name: str,
        function: int,
        link: _core.UacLink,
        mic: bool,
        wait: Waiter,
        post: Poster,
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.host.audio().speaker` or `.mic` instead."""
        self.name = name
        self.function = function
        self._link = link
        self._mic = mic
        self._wait = wait
        self._post = post
        self._pending: set[int] = set()

    @property
    def bound(self) -> bool:
        """Whether the host has bound its audio driver to the function and found this stream."""
        return self._link.bound() and self._record().present

    @property
    def interface(self) -> int | None:
        """bInterfaceNumber of the audio streaming interface, None while not found."""
        record = self._record()
        return record.interface if record.present else None

    @property
    def alternate(self) -> int | None:
        """The alternate setting `start` selects, None while not found."""
        record = self._record()
        return record.alternate if record.present else None

    @property
    def format(self) -> UacFormat | None:
        """The PCM format of that setting, None while not found."""
        record = self._record()
        if not record.present:
            return None
        return UacFormat(record.channels, record.subslot, record.bits)

    @property
    def sample_rates(self) -> tuple[int, ...]:
        """Sample rates of the clock's GET_RANGE in Hz, empty before it is read. A continuous
        range shows as its two ends."""
        return tuple(self._record().rates)

    @property
    def rate(self) -> int:
        """The rate the stream runs at in Hz: the clock's GET_CUR, else the highest of
        `sample_rates`, 0 before the clock is read."""
        return self._record().rate

    @property
    def running(self) -> bool:
        """Whether the setting is selected and an isochronous packet goes every frame."""
        return self._record().running

    async def start(self) -> VirtualTime:
        """Select the stream's alternate setting and run the machine until the device
        acknowledged it, the virtual time that was seen as the result. The first isochronous
        packet goes 2 frames after the pipe opens.

        It sends what Linux sends when it prepares a stream: the clock's validity and GET_CUR
        of its frequency, SET_CUR when the rate differs and the clock takes it, the validity
        again, then SET_INTERFACE. A clock another running stream holds is not read again. A
        running stream returns at once.

        Raises:
            UsbError: No audio driver is bound, status `unbound`, the function has no stream in
                this direction, `absent`, its clock rate is unknown, `no_rate`, a start is in
                progress or the other stream runs on the same clock at another rate, `busy`, or a
                request ended neither completed, stalled nor timed out.
            UsbStall: The device stalled SET_INTERFACE or SET_CUR.
            UsbTimeout: A request did not end within the host's timeout.
            EmulationError: The run ended before the device acknowledged.
            RuntimeError: Called from a handler or a model.
        """
        if self.running:
            _, now = await self._post(None)
            return now
        link, mic = self._link, self._mic
        operation, posted_at = await self._post(link.command_start(mic))
        if operation == 0:
            if not self._link.bound():
                status = "unbound"
            elif not self._record().present:
                status = "absent"
            elif self._record().rate == 0:
                status = "no_rate"
            else:
                status = "busy"
            raise UsbError(f"{self._where}: cannot start, {status}", status=status)
        result, done_at = await await_result(
            self._link,
            self._pending,
            self._wait,
            operation,
            posted_at,
            f"{self._where}: the run ended before the start did",
        )
        raise_for(result, f"{self._where}: start")
        return done_at

    def stop(self, *, at: VirtualTime | None = None) -> Pending[VirtualTime]:
        """Stop the stream, posted at the call as one command: at once while the machine is
        parked, at the next block boundary while it runs, or at the first one at or after `at`.
        Await the result for the virtual time it ran at.

        The host closes the pipe then: no isochronous packet goes from then on, and a speaker's
        samples not yet sent are dropped, whether it ran or not. SET_INTERFACE 0 is queued in the
        host state and goes on the bus as the machine runs, after any control request already
        queued. A start in progress is cancelled. A stream neither running nor starting sends no
        request.

        Raises:
            RuntimeError: Called from a handler or a model.
            ValueError: `at` before the time of a parked machine, from the await when running.
        """
        link, mic = self._link, self._mic
        return written(self._post(link.command_stop(mic), at))

    def discard(self) -> None:
        """Forget the starts waited for, at a restore."""
        self._pending.clear()

    @property
    def _where(self) -> str:
        return f"{self.name} audio function {self.function} {'mic' if self._mic else 'speaker'}"

    def _record(self) -> _core.UacStreamRecord:
        return self._link.stream(self._mic)


class UacSpeaker(UacStream):
    """The speaker of a USB Audio 2.0 function, `host.audio().speaker`: PCM to the device on
    its isochronous OUT endpoint.

    Each frame while running carries the rate's samples per channel, rate / 1000, a fraction
    spread as USB Audio 2.0 3.16.2 does: at 44.1 kHz 44 samples, one frame in 10 carries 45.
    Samples written fill the packets in order, silence pads a packet past the last one. They
    wait while the speaker is stopped, are dropped by `stop`, and are model state: a snapshot
    keeps them, bound or not, and a restore puts them back.
    """

    __slots__ = ()

    @property
    def queued(self) -> int:
        """Samples written and not yet put in a packet, interleaved values."""
        return self._link.queued()

    @property
    def sending(self) -> bool:
        """Whether a packet carrying written samples waits to go on the bus. The first packet
        of a stream waits two frames after `start` opens the pipe."""
        return self._link.sending()

    @property
    def sent_at(self) -> VirtualTime | None:
        """Virtual time of the last packet that carried written samples, None before any."""
        at = self._link.sent_at()
        return None if at is None else VirtualTime(at)

    def write(
        self, samples: Sequence[int], *, at: VirtualTime | None = None
    ) -> Pending[VirtualTime]:
        """Queue samples for the device, interleaved, a whole number of frames of audio. Posted
        at the call as one command, as `UartStream.write`: await the result for the time they
        were queued at.

        Args:
            samples: Signed samples of the format's subslot, `bytes_per_sample` bytes, as the bus
                carries them. Before the format is known any 32-bit value is taken and its low
                bytes are sent.
            at: The first block boundary at or after it queues them, None for the next one.

        Raises:
            ValueError: A sample is out of range, or the count is not a multiple of the channels.
            RuntimeError: Called from a handler or a model.
        """
        values = list(samples)
        layout = self.format
        width = 4 if layout is None else layout.bytes_per_sample
        top = 1 << (8 * width - 1)
        for sample in values:
            if isinstance(sample, bool) or not -top <= sample < top:
                raise ValueError(f"a {width}-byte sample is {-top} to {top - 1}, not {sample!r}")
        if layout is not None and len(values) % layout.channels:
            raise ValueError(
                f"{len(values)} samples are not whole frames of {layout.channels} channels"
            )
        link = self._link
        return written(self._post(link.command_write(values), at))

    async def drain(
        self,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> None:
        """Run the machine until every sample written went out in an isochronous packet, or
        `for_` elapsed, as `UartStream.read` runs it. `sent_at` is then the time of the packet
        that carried the last sample. The host sends the first packet of a stream two frames
        after `start` opens the pipe, so a drain right after `start` waits for it.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`. Already
        past, the wait ends at once.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            UsbError: Samples are queued and the speaker is not running, or while the call
                waited a `stop` dropped samples, or a `stop`, an unplug or a bus reset dropped
                the packet not sent yet. Status `stopped`.
            UsbTimeout: Samples are still queued or in a packet not sent yet when the run ends:
                `for_` elapsed, `deadline` passed or the core went idle. Status `queued`.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
        """
        check_wait(for_, deadline)
        flushes = self._link.flushes()
        if self._link.queued() and not self.running:
            raise UsbError(
                f"{self._where}: {self._link.queued()} samples queued and the speaker stopped",
                status="stopped",
            )
        if self._unsent():
            await self._wait(self._link.predicate_drained(flushes), for_, deadline)
        if self._link.flushes() != flushes:
            raise UsbError(f"{self._where}: stopped while draining", status="stopped")
        if self._unsent():
            unsent = (
                f"{self._link.queued()} samples still queued"
                if self._link.queued()
                else "the last packet not sent yet"
            )
            raise UsbTimeout(f"{self._where}: {unsent}", status="queued")

    def _unsent(self) -> bool:
        return self._link.queued() != 0 or self._link.sending()

    @override
    def __repr__(self) -> str:
        return f"UacSpeaker({self.name}, {self.function})"


class UacMic(UacStream):
    """The microphone of a USB Audio 2.0 function, `host.audio().mic`: PCM from the device on
    its isochronous IN endpoint.

    While running, the host asks for one packet of wMaxPacketSize every frame and keeps each as
    a `UacFrame` with its time, up to 65536 frames, the oldest dropped first and counted in
    `dropped`. A frame the device left empty or did not answer is kept as a gap, with no
    samples, so frames stay 1 ms apart. `Machine.restore` drops the frames not read yet.
    """

    __slots__ = ()

    @property
    def available(self) -> int:
        """Frames received, not read yet."""
        return self._link.available()

    @property
    def dropped(self) -> int:
        """Frames dropped because the queue held its limit, since the machine was built or the
        last `Machine.restore`."""
        return self._link.dropped()

    def clear(self) -> None:
        """Drop the frames received and not read yet, at once, as a restore does, from any
        thread, the machine running or not. The next frame read is newer than the call."""
        self._link.clear()

    async def read_frames(
        self,
        n: int,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> list[UacFrame]:
        """Run the machine until `n` frames arrived, or `for_` elapsed, and take them, as
        `AudioStream.read_frames`.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`, and caps
        what the read takes: no frame the host queued past it, even one queued at the call.
        Already past, the read ends at once with the frames queued by it.

        A read that waits takes only the frames the host queued by the time its wait stopped,
        as `HidStream.read_reports` does: a frame taken can carry an `at` up to about 1 ms past
        that time, and later frames stay queued for the next read.

        Returns:
            Up to `n` frames, oldest first, fewer when `for_` elapsed, `deadline` passed or
                the core went idle.

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
            return _uac_frames(self._link.drain_until(n, cutoff(None, deadline)))
        stop = await self._wait(self._link.predicate_available(n), for_, deadline)
        return _uac_frames(self._link.drain_until(n, cutoff(stop, deadline)))

    def take_frames(self) -> list[UacFrame]:
        """Every frame received and not read yet, at once, never running the machine. Allowed
        parked and running, from any thread. A frame arriving during the call stays queued for
        the next one. Unlike `clear`, the frames are returned. A take while a `read_frames` of
        the same stream waits can leave that read short."""
        link = self._link
        return _uac_frames(link.drain(link.available()))

    @override
    def __repr__(self) -> str:
        return f"UacMic({self.name}, {self.function})"


class UacFunction:
    """A USB Audio 2.0 function of a USB device, `m.periph.OTG_FS.host.audio()`: a speaker, a
    microphone and feature units, in virtual time.

    The host's audio driver binds each audio control interface of protocol 0x20 with the
    interfaces of its association. At configuration it probes as Linux snd-usb-audio does:
    for each alternate setting of each streaming interface, GET_RANGE of the clock's
    frequency, SET_INTERFACE 0, the clock's validity and frequency, SET_CUR of the highest
    rate when the clock takes it, SET_INTERFACE to the setting, then SET_INTERFACE 0. Then, for
    each feature unit an output terminal reaches, mute and volume: GET_RANGE of the volume, the
    resolution check Linux makes with SET_CUR, and GET_CUR of each channel. A failed or stalled
    request is not fatal, as in Linux: a stalled SET_CUR of the frequency leaves the clock's
    rate. The function outlives the driver: an unplug and a replug bind the same streams.

    Not modelled from Linux: a clock selector is followed through its first pin, with no GET_CUR
    or SET_CUR on it. Only mute and volume of feature units are probed: other controls, mixer,
    selector, processing and extension units and terminal connector controls send nothing. The
    early exit of Linux's mixer on an unrecoverable dB range is not mirrored. A setting whose
    isochronous endpoint has a bInterval above 1 is not streamed.
    """

    __slots__ = (
        "__weakref__",
        "_link",
        "_pending",
        "_post",
        "_wait",
        "function",
        "mic",
        "name",
        "speaker",
    )

    def __init__(
        self,
        name: str,
        function: int,
        link: _core.UacLink,
        wait: Waiter,
        post: Poster,
    ) -> None:
        """Wrap a native link. Use `m.periph.<NAME>.host.audio()` instead."""
        self.name = name
        self.function = function
        self._link = link
        self._wait = wait
        self._post = post
        self._pending: set[int] = set()
        self.speaker = UacSpeaker(name, function, link, False, wait, post)
        self.mic = UacMic(name, function, link, True, wait, post)

    @property
    def bound(self) -> bool:
        """Whether the host has bound its audio driver to the function.

        True from the moment the device is configured, before the probe requests: rates and
        feature unit values are not read yet. `ready` tells when they are. False again once the
        device is unplugged, reset by the bus or restored to a snapshot taken before
        configuration.
        """
        return self._link.bound()

    @property
    def ready(self) -> bool:
        """Whether the function is bound and the driver's probe requests have ended, whatever
        their status. From then on `sample_rates`, `rate` and `feature_units` hold what the
        device answered."""
        return self._link.bound() and self._link.ready()

    @property
    def feature_units(self) -> tuple[UacFeatureUnit, ...]:
        """Feature units in the order the probe reached them, empty before configuration."""
        return tuple(
            UacFeatureUnit(
                unit=record.id,
                channels=record.channels,
                controls=tuple(record.controls),
                mute=tuple(record.mute),
                volume=tuple(record.volume),
                volume_range=tuple(
                    None if low is None or high is None or step is None else (low, high, step)
                    for low, high, step in zip(
                        record.volume_min, record.volume_max, record.volume_res, strict=True
                    )
                ),
            )
            for record in self._link.units()
        )

    async def wait_ready(
        self,
        *,
        for_: VirtualDuration | None = None,
        deadline: VirtualTime | None = None,
    ) -> None:
        """Run the machine until `ready`, or `for_` elapsed, as `CdcStream.wait_ready`.

        `deadline` stops waiting at this virtual time instead, exclusive with `for_`. Already
        past, the wait ends at once.

        Raises:
            TypeError: Both `for_` and `deadline` given, or `deadline` is no `VirtualTime`.
            UsbTimeout: The function is not ready when the run ends: `for_` elapsed,
                `deadline` passed or the core went idle. Its status is `unbound` while no
                driver is bound, else `not_ready`.
            EmulationError: The run stopped for another reason, such as a fault, or, inside
                `running()`, the free run ended by itself, a handler's USER stop included.
        """
        check_wait(for_, deadline)
        if not self.ready:
            await self._wait(self._link.predicate_ready(), for_, deadline)
        if not self.ready:
            status = "not_ready" if self.bound else "unbound"
            raise UsbTimeout(f"{self._where}: not ready, {status}", status=status)

    async def set_mute(self, unit: int, channel: int, mute: bool) -> VirtualTime:
        """Send SET_CUR of a feature unit's mute and run the machine until it ends, the virtual
        time it was seen ended as the result.

        Args:
            unit: bUnitID.
            channel: 0 for the master channel, n for the n-th logical channel.
            mute: The mute.

        Raises:
            ValueError: The function has no such unit or channel, or its mute is not writable.
            UsbError: No audio driver is bound, or the request ended neither completed, stalled
                nor timed out. Linux sends a failed SET_CUR up to 10 times, so does the host.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from a handler or a model.
        """
        return await self._set(unit, channel, _MUTE, int(bool(mute)), f"mute {int(bool(mute))}")

    async def set_volume(self, unit: int, channel: int, volume: int) -> VirtualTime:
        """Send SET_CUR of a feature unit's volume and run the machine until it ends, as
        `set_mute`.

        Args:
            unit: bUnitID.
            channel: 0 for the master channel, n for the n-th logical channel.
            volume: In 1/256 dB, -32768 to 32767.

        Raises:
            ValueError: The volume is out of range, the function has no such unit or channel,
                or its volume is not writable.
            UsbError: As `set_mute`.
            UsbStall: The device answered STALL.
            UsbTimeout: The request did not end within the host's timeout.
            RuntimeError: Called from a handler or a model.
        """
        if not -0x8000 <= volume <= 0x7FFF:
            raise ValueError(f"a volume is -32768 to 32767, not {volume}")
        return await self._set(unit, channel, _VOLUME, volume, f"volume {volume}")

    def discard(self) -> None:
        """Forget the requests and starts waited for, at a restore."""
        self._pending.clear()
        self.speaker.discard()
        self.mic.discard()

    @property
    def _where(self) -> str:
        return f"{self.name} audio function {self.function}"

    async def _set(
        self, unit: int, channel: int, control: int, value: int, label: str
    ) -> VirtualTime:
        where = f"{self._where}: SET_CUR unit {unit} channel {channel} {label}"
        if not self._link.bound():
            raise UsbError(f"{where} with no audio driver bound", status="unbound")
        found = next((u for u in self.feature_units if u.unit == unit), None)
        if found is None or not 0 <= channel <= found.channels:
            raise ValueError(f"{where}: no such unit and channel")
        if not (found.controls[channel] >> (2 * (control - 1))) & 2:
            raise ValueError(f"{where}: the control is not writable")
        link = self._link
        operation, posted_at = await self._post(
            link.command_set_feature(unit, channel, control, value)
        )
        if operation == 0:
            status = "refused" if self._link.bound() else "unbound"
            raise UsbError(f"{where} refused by the driver, {status}", status=status)
        result, done_at = await await_result(
            self._link,
            self._pending,
            self._wait,
            operation,
            posted_at,
            f"{where}: the run ended before it did",
        )
        raise_for(result, where)
        return done_at

    @override
    def __repr__(self) -> str:
        return f"UacFunction({self.name}, {self.function})"


def _uac_frames(records: Iterable[_core.UacFrameRecord]) -> list[UacFrame]:
    return [UacFrame(tuple(record.samples), VirtualTime(record.at_ps)) for record in records]
