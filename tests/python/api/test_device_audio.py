"""An SAI block owned by a bus device: the device hears the frames a transmitting guest block
sends at their end stamps, equal to an `AudioStream` capture of the same run, and queues the words
a receiving guest block takes."""

from __future__ import annotations

import asyncio
import contextlib
import weakref

import popcorn
import pytest
from popcorn import ConfigError, ModelError, System
from popcorn.board import AudioFrame, DeviceAudio, UartDevice
from popcorn.units import ms, us

from .conftest import collected
from .test_audio import BUDGET, FRAME, FRAMES, SAI_ELF, at_main, log, run_entry
from .test_system_loop import links
from .test_system_loop import member as system_member
from .test_virtual_loop import stamp


class Codec(UartDevice):
    def __init__(self, *blocks: str, words: list[int] | None = None) -> None:
        super().__init__()
        self.blocks = blocks
        self.words = words
        self.frames: list[AudioFrame] = []
        self.stamps: list[int] = []
        self.ports: dict[str, DeviceAudio] = {}

    def on_attach(self) -> None:
        for block in self.blocks:
            self.ports[block] = self.board.audio_port("SAI1", block, self.heard)  # type: ignore[arg-type]
        if self.words is not None:
            self.ports["B"].write(self.words)

    def heard(self, frame: AudioFrame) -> None:
        self.frames.append(frame)
        self.stamps.append(stamp(self.loop))

    def snapshot(self) -> bytes:
        return b""

    def restore(self, data: bytes) -> None:
        self.on_attach()


def machine(u575: popcorn.PartDescription, codec: UartDevice | None = None) -> popcorn.Machine:
    devices = None if codec is None else {"USART1": [codec]}
    m = popcorn.Machine(u575, image=popcorn.Image.elf(SAI_ELF), devices=devices)
    at_main(m)
    return m


def captured(u575: popcorn.PartDescription) -> list[AudioFrame]:
    m = machine(u575)
    stream = m.periph.SAI1.audio("A")
    run_entry(m, "duplex_irq")
    return asyncio.run(stream.read_frames(stream.available))


def test_frames_a_device_hears_equal_an_audio_stream_capture_of_the_same_run(
    u575: popcorn.PartDescription,
) -> None:
    codec = Codec("A")
    m = machine(u575, codec)
    assert run_entry(m, "duplex_irq") == [FRAMES, FRAMES]
    reference = captured(u575)
    assert len(codec.frames) == len(reference) > FRAMES // 2
    assert codec.frames == reference
    assert [seen - frame.at.ps for seen, frame in zip(codec.stamps, codec.frames, strict=True)] == [
        FRAME.ps
    ] * len(reference)


def test_two_runs_give_the_same_frames_and_stamps(u575: popcorn.PartDescription) -> None:
    def once() -> tuple[list[AudioFrame], list[int]]:
        codec = Codec("A")
        run_entry(machine(u575, codec), "duplex_irq")
        return codec.frames, codec.stamps

    assert once() == once()


def test_words_a_device_writes_reach_the_firmware_buffer_in_order(
    u575: popcorn.PartDescription,
) -> None:
    words = [0xA000 + index for index in range(FRAMES)]
    codec = Codec("B", words=words)
    m = machine(u575, codec)
    assert run_entry(m, "duplex_irq") == [FRAMES, FRAMES]
    received = m.var("received")
    assert [int(received[index].value) for index in range(FRAMES)] == words
    assert (codec.ports["B"].queued, codec.ports["B"].starved) == (0, 1)


def test_a_receiving_block_with_no_word_queued_gets_zero_and_counts_the_frame_starved(
    u575: popcorn.PartDescription,
) -> None:
    codec = Codec("B", words=[0xB000 + index for index in range(10)])
    m = machine(u575, codec)
    assert run_entry(m, "duplex_irq") == [FRAMES, FRAMES]
    received = m.var("received")
    values = [int(received[index].value) for index in range(FRAMES)]
    assert values[:10] == [0xB000 + index for index in range(10)]
    assert values[10:] == [0] * (FRAMES - 10)
    assert codec.ports["B"].starved == len(captured(u575)) - 5


def test_a_frame_costs_one_gil_entry_for_the_hook(u575: popcorn.PartDescription) -> None:
    def entries(listening: bool) -> int:
        codec = Codec("A") if listening else None
        m = machine(u575, codec)
        before = m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]
        run_entry(m, "duplex_irq")
        return m._handle.gil_entries() - before  # pyright: ignore[reportPrivateUsage]

    assert entries(True) - entries(False) == len(captured(u575))


def test_a_block_has_one_owner(u575: popcorn.PartDescription) -> None:
    first = Codec("A")
    second = Codec()
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(SAI_ELF),
        devices={"USART1": [first], "USART2": [second]},
    )
    at_main(m)
    asyncio.run(m.run(for_=us(10)))
    with pytest.raises(ConfigError, match="no stream"):
        m.periph.SAI1.audio("A")
    second.board.audio_port("SAI1", "B", second.heard if hasattr(second, "heard") else print)
    with pytest.raises(ConfigError, match="owner"):
        second.board.audio_port("SAI1", "A", print)
    first.board.audio_port("SAI1", "A", first.heard)

    taken = machine(u575, Codec())
    taken.periph.SAI1.audio("B")
    with pytest.raises(ConfigError, match="owner"):
        next(iter(taken._devices["USART1"])).board.audio_port("SAI1", "B", print)  # type: ignore[attr-defined]


class Plain(UartDevice):
    def __init__(self) -> None:
        super().__init__()
        self.port: DeviceAudio | None = None

    def on_attach(self) -> None:
        self.port = self.board.audio_port("SAI1", "A", lambda frame: None)


def test_a_frame_heard_counts_as_traffic_so_the_default_snapshot_refuses(
    u575: popcorn.PartDescription,
) -> None:
    quiet = Plain()
    m = machine(u575, quiet)
    asyncio.run(m.run(for_=us(1)))
    m.snapshot()
    run_entry(m, "duplex_irq")
    with pytest.raises(popcorn.SnapshotRefused, match="override snapshot"):
        m.snapshot()


def test_a_restore_drops_the_queued_words_and_the_starved_count(
    u575: popcorn.PartDescription,
) -> None:
    codec = Codec("B", words=[1, 2, 3])
    m = machine(u575, codec)
    asyncio.run(m.run(for_=us(10)))
    saved = m.snapshot()
    run_entry(m, "duplex_irq")
    assert codec.ports["B"].starved > 0
    codec.ports["B"].write([9] * 4)
    m.restore(saved)
    assert codec.ports["B"].queued == 3
    assert codec.ports["B"].starved == 0


def test_a_part_with_no_sai_refuses() -> None:
    from popcorn.stm32 import mcu

    codec = Codec("A")
    description = mcu("STM32L152RETx")
    m = popcorn.Machine(description, devices={"USART1": [codec]})
    m.reset()
    with pytest.raises(ModelError):
        asyncio.run(m.run(for_=us(10)))


class Late(UartDevice):
    def __init__(self) -> None:
        super().__init__()
        self.frames: list[AudioFrame] = []

    def claim(self) -> None:
        self.board.audio_port("SAI1", "A", self.frames.append)


def test_frames_sent_before_the_claim_are_dropped_and_the_next_one_is_the_one_heard(
    u575: popcorn.PartDescription,
) -> None:
    late = Late()
    m = machine(u575, late)
    m.reg.pc = m.sym("duplex_irq")
    asyncio.run(m.run(for_=us(1500)))
    claimed = m.time
    late.claim()
    asyncio.run(m.run(for_=us(1000)))
    reference = captured(u575)
    assert late.frames
    assert all(frame in reference for frame in late.frames)
    assert late.frames[0].at.ps >= claimed.ps - FRAME.ps


def test_frames_heard_again_after_a_restore_equal_the_first_run(
    u575: popcorn.PartDescription,
) -> None:
    codec = Codec("A")
    m = machine(u575, codec)
    asyncio.run(m.run(for_=us(10)))
    saved = m.snapshot()
    run_entry(m, "duplex_irq")
    first = list(codec.frames)
    assert first
    codec.frames.clear()
    codec.stamps.clear()
    m.restore(saved)
    run_entry(m, "duplex_irq")
    assert codec.frames == first


def test_a_write_from_the_test_thread_on_a_running_machine_is_refused_naming_post(
    u575: popcorn.PartDescription,
) -> None:
    codec = Codec("B")
    m = machine(u575, codec)
    asyncio.run(m.run(for_=us(10)))
    m.reg.pc = m.sym("duplex_irq") & ~1

    async def go() -> None:
        async with m.running():
            with pytest.raises(popcorn.MachineRunning, match=r"loop\.post\(audio\.write"):
                codec.ports["B"].write([1, 2, 3])

    with contextlib.suppress(popcorn.EmulationError):
        asyncio.run(go())
    assert codec.ports["B"].queued == 0


def posted_write(u575: popcorn.PartDescription, *, running: bool) -> list[int]:
    words = [0xC000 + index for index in range(FRAMES)]
    codec = Codec("B")
    m = machine(u575, codec)
    asyncio.run(m.run(for_=us(10)))
    m.reg.pc = m.sym("duplex_irq") & ~1
    start = m.time

    async def go() -> None:
        if running:
            pending = m.loop.post(codec.ports["B"].write, words, at=start + us(300))
            async with m.running():
                await pending
                await asyncio.sleep(0.2)
        else:
            pending = m.loop.post(codec.ports["B"].write, words, at=start + us(300))
            await m.run(for_=ms(6))
            await pending

    with contextlib.suppress(popcorn.EmulationError):
        asyncio.run(go())
    received = m.var("received")
    return [int(received[index].value) for index in range(FRAMES)]


def test_a_posted_write_gives_equal_buffers_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    parked = posted_write(u575, running=False)
    assert any(parked)
    assert posted_write(u575, running=False) == parked
    assert posted_write(u575, running=True) == parked
    assert posted_write(u575, running=True) == parked


class Writer(UartDevice):
    def __init__(self, *, in_attach: bool) -> None:
        super().__init__()
        self.in_attach = in_attach
        self.port: DeviceAudio | None = None

    def on_attach(self) -> None:
        self.port = self.board.audio_port("SAI1", "B", lambda frame: None)
        if self.in_attach:
            self.port.write([1, 2, 3])


def test_a_write_outside_on_attach_is_traffic_and_one_inside_is_not(
    u575: popcorn.PartDescription,
) -> None:
    quiet = Writer(in_attach=True)
    m = machine(u575, quiet)
    asyncio.run(m.run(for_=us(1)))
    saved = m.snapshot()
    m.restore(saved)
    asyncio.run(m.run(for_=us(1)))
    assert quiet.port is not None
    assert quiet.port.queued == 3

    later = Writer(in_attach=False)
    m = machine(u575, later)
    asyncio.run(m.run(for_=us(1)))
    m.snapshot()
    assert later.port is not None
    later.port.write([1, 2, 3])
    with pytest.raises(popcorn.SnapshotRefused, match="override snapshot"):
        m.snapshot()


class Keeper(UartDevice):
    def __init__(self) -> None:
        super().__init__()
        self.frames: list[AudioFrame] = []

    def on_attach(self) -> None:
        self.board.audio_port("SAI1", "A", self.frames.append)

    def snapshot(self) -> bytes:
        return b""

    def restore(self, data: bytes) -> None:
        return None


def test_a_restore_keeps_the_block_owned_for_streams_and_the_hook_installed(
    u575: popcorn.PartDescription,
) -> None:
    keeper = Keeper()
    m = machine(u575, keeper)
    asyncio.run(m.run(for_=us(10)))
    saved = m.snapshot()
    m.restore(saved)
    with pytest.raises(ConfigError, match="no stream"):
        m.periph.SAI1.audio("A")
    run_entry(m, "duplex_irq")
    assert len(keeper.frames) > FRAMES // 2


def test_a_reclaiming_restore_leaves_one_callback_table_entry(
    u575: popcorn.PartDescription,
) -> None:
    codec = Codec("A")
    m = machine(u575, codec)
    asyncio.run(m.run(for_=us(10)))
    saved = m.snapshot()
    table = m._callback_table  # pyright: ignore[reportPrivateUsage]
    size = len(table)
    for _ in range(1000):
        m.restore(saved)
    assert len(table) == size


def test_a_closed_loop_drops_the_frames(u575: popcorn.PartDescription) -> None:
    codec = Codec("A")
    m = machine(u575, codec)
    asyncio.run(m.run(for_=us(10)))
    m.loop.close()
    m.reg.pc = m.sym("duplex_irq") & ~1
    with contextlib.suppress(popcorn.EmulationError):
        asyncio.run(m.run(budget=100_000_000))
    assert codec.frames == []


@pytest.mark.parametrize("quantum", [None, 5, 1])
def test_a_device_on_a_member_hears_the_lone_machines_frames_and_serves_its_words(
    u575: popcorn.PartDescription, quantum: int | None
) -> None:
    words = [0xA000 + index for index in range(FRAMES)]

    def served(codec: Codec, a: popcorn.Machine) -> tuple[list[int], int]:
        received = a.var("received")
        return [int(received[index].value) for index in range(FRAMES)], codec.ports["B"].starved

    def lone() -> tuple[Codec, tuple[list[int], int]]:
        codec = Codec("A", "B", words=words)
        a = popcorn.Machine(
            u575, image=popcorn.Image.elf(SAI_ELF), devices={"USART1": [codec]}, name="a"
        )
        at_main(a)
        a.reg.pc = a.sym("duplex_irq")
        asyncio.run(a.run(for_=ms(6)))
        return codec, served(codec, a)

    def member() -> tuple[Codec, tuple[list[int], int]]:
        codec = Codec("A", "B", words=words)
        a = popcorn.Machine(
            u575, image=popcorn.Image.elf(SAI_ELF), devices={"USART1": [codec]}, name="a"
        )
        at_main(a)
        a.reg.pc = a.sym("duplex_irq")
        with System({"a": a, "b": system_member(u575, "b")}, links(quantum)) as system:
            asyncio.run(system.run(for_=ms(6)))
            return codec, served(codec, a)

    reference, (reference_words, reference_starved) = lone()
    assert len(reference.frames) > 30
    assert reference_words == words
    heard, (heard_words, heard_starved) = member()
    assert heard.frames == reference.frames
    assert heard.stamps == reference.stamps
    assert heard_words == reference_words
    assert heard_starved == reference_starved


def reconfigured(m: popcorn.Machine, **layout: int) -> None:
    """Run `duplex_irq` to the first enable of block A and set a layout on it. The layouts obey
    RM0456 69.4.5: DS at most SLOTSZ, slots times SLOTSZ at most FRL + 1. Both legs of a case use
    this procedure, so it compares a device with a capture under one sequence, and a frame the
    old layout was sending ends first."""
    m.reg.pc = m.sym("duplex_irq")
    asyncio.run(
        m.run(budget=BUDGET, stop_when=lambda machine: machine.periph.SAI1.ACR1.SAIAEN == 1)
    )
    sai = m.periph.SAI1
    sai.ACR1.poke(SAIAEN=0)
    if "nbslot" in layout:
        sai.ASLOTR.poke(NBSLOT=layout["nbslot"], SLOTEN=layout["slots"])
        sai.AFRCR.poke(FRL=layout["frl"], FSALL=layout["fsall"])
    if "ds" in layout:
        sai.ACR1.poke(DS=layout["ds"])
        sai.ASLOTR.poke(SLOTSZ=layout["slotsz"])
        sai.AFRCR.poke(FRL=layout["frl"], FSALL=layout["fsall"])
    sai.ACR1.poke(SAIAEN=1)


@pytest.mark.parametrize(
    ("layout", "slots", "data_bits"),
    [
        (dict(nbslot=3, slots=0xF, frl=63, fsall=31), 4, 16),
        (dict(ds=7, slotsz=2, frl=63, fsall=31), 2, 32),
    ],
    ids=["four_slots", "32_bits"],
)
def test_a_device_hears_a_block_of_four_slots_or_32_bits_as_a_capture_does(
    u575: popcorn.PartDescription, layout: dict[str, int], slots: int, data_bits: int
) -> None:
    codec = Codec("A")
    m = machine(u575, codec)
    reconfigured(m, **layout)
    codec.frames.clear()
    asyncio.run(m.run(for_=ms(3), budget=BUDGET))
    reference = machine(u575)
    stream = reference.periph.SAI1.audio("A")
    reconfigured(reference, **layout)
    asyncio.run(reference.run(for_=ms(3), budget=BUDGET))
    expected = asyncio.run(stream.read_frames(stream.available))

    def new_layout(frames: list[AudioFrame]) -> list[AudioFrame]:
        return [
            frame for frame in frames if (len(frame.slots), frame.data_bits) == (slots, data_bits)
        ]

    heard = new_layout(codec.frames)
    assert len(heard) > 10
    assert len(heard) >= len(codec.frames) - 1
    assert heard == new_layout(expected)[-len(heard) :]


class Raising(Codec):
    def __init__(self, *blocks: str, at: int) -> None:
        super().__init__(*blocks)
        self.at = at

    def heard(self, frame: AudioFrame) -> None:
        super().heard(frame)
        if len(self.frames) == self.at:
            raise KeyError("a bug in the device")


def test_a_raising_on_frame_stops_the_run_and_the_block_keeps_sending(
    u575: popcorn.PartDescription,
) -> None:
    codec = Raising("A", at=5)
    m = machine(u575, codec)
    m.reg.pc = m.sym("duplex_irq")
    with pytest.raises(popcorn.ModelError, match="KeyError"):
        asyncio.run(m.run(budget=BUDGET))
    assert len(codec.frames) == 5
    stop = asyncio.run(m.run(budget=BUDGET))
    assert stop.reason is popcorn.StopReason.USER
    assert log(m) == [FRAMES, FRAMES]
    assert codec.frames == captured(u575)


def test_queued_and_starved_read_the_same_after_the_machine_was_collected(
    u575: popcorn.PartDescription,
) -> None:
    codec = Codec("B", words=[0xB000 + index for index in range(10)])
    m = machine(u575, codec)
    run_entry(m, "duplex_irq")
    port = codec.ports["B"]
    expected = (port.queued, port.starved)
    assert expected[1] > 0
    watch = weakref.ref(m)
    del m
    assert collected(watch)
    assert (port.queued, port.starved) == expected
    with pytest.raises(ConfigError):
        port.write([1])
