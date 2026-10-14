"""SAI audio streams: frames a block transmits, captured from Python in virtual time.

The LL test firmware's SAI1 block A is a 16-bit stereo master whose frame lasts 64 us, the
native SAI test's frame time. A frame's `at` is its start."""

from __future__ import annotations

import asyncio
import itertools
import logging
import struct
from collections.abc import Mapping
from pathlib import Path
from typing import Literal, cast

import popcorn
import pytest
from popcorn import StopReason
from popcorn.board import AudioFrame, AudioLayout, AudioOptions, AudioStream, I2CTarget
from popcorn.units import VirtualDuration, VirtualTime, ms, us

from .conftest import late_drained_read, run_bounded
from .projects import Silent

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
SAI_ELF = PREBUILT / "sai_ll.elf"
BUDGET = 100_000_000
FRAMES = 64
FRAME = us(64)
SLAVE_FRAME = VirtualDuration(62_500_000)
STEREO_FRAMES = 512
CODEC_FRAMES = 1400
TIM2_CNT = 0x4000_0024
I2S_LAYOUT = AudioLayout(
    frame_bits=32,
    slot_bits=16,
    first_bit_offset=0,
    fs_definition=True,
    fs_offset=True,
    fs_active_high=False,
    fs_active_bits=16,
    slot_starts=(1, 17),
)

type Block = Literal["A", "B"]


def ramp(index: int) -> int:
    return 0x1000 + index


def log(m: popcorn.Machine) -> list[int]:
    count = m.mem.u32[m.sym("log_len")]
    raw = m.mem.read(m.sym("log_buf"), 4 * min(count, 256))
    return list(struct.unpack(f"<{len(raw) // 4}I", raw))


def at_main(m: popcorn.Machine) -> None:
    m.reset()
    stop = asyncio.run(m.run(until="main", budget=BUDGET))
    assert stop.reason is StopReason.ADDRESS, stop


def run_entry(m: popcorn.Machine, entry: str) -> list[int]:
    m.reg.pc = m.sym(entry)
    stop = asyncio.run(m.run(budget=BUDGET))
    assert stop.reason is StopReason.USER, stop
    return log(m)


def sai_machine(
    u575: popcorn.PartDescription, options: dict[Block, AudioOptions] | None = None
) -> popcorn.Machine:
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(SAI_ELF),
        audio=None if options is None else {"SAI1": options},
    )
    at_main(m)
    return m


def read_all(stream: AudioStream) -> list[tuple[int, ...]]:
    return [frame.slots for frame in asyncio.run(stream.read_frames(stream.available))]


def test_frames_a_block_sent_before_the_stream_was_taken_are_captured(
    u575: popcorn.PartDescription,
) -> None:
    m = sai_machine(u575)
    assert run_entry(m, "tx_poll") == [0]

    stream = m.periph.SAI1.audio("A")
    assert stream.available == FRAMES
    frames = asyncio.run(stream.read_frames(FRAMES, for_=ms(1)))
    assert [frame.slots for frame in frames] == [
        (ramp(2 * index), ramp(2 * index + 1)) for index in range(FRAMES)
    ]
    assert all(later.at - earlier.at == FRAME for earlier, later in itertools.pairwise(frames))
    assert all(
        (frame.data_bits, frame.active_slots, frame.mono) == (16, 0b11, False) for frame in frames
    )
    assert frames[0].layout == I2S_LAYOUT
    assert all(frame.layout is frames[0].layout for frame in frames)
    assert (stream.available, stream.dropped) == (0, 0)


def test_a_receiving_block_takes_the_words_written_in_slot_order(
    u575: popcorn.PartDescription,
) -> None:
    m = sai_machine(u575)
    words = [0xA000 + index for index in range(FRAMES)]
    receiver = m.periph.SAI1.audio("B")
    asyncio.run(receiver.write(words))
    assert receiver.queued == FRAMES

    assert run_entry(m, "duplex_irq") == [FRAMES, FRAMES]
    received = m.var("received")
    assert [received[index].value for index in range(FRAMES)] == words
    assert receiver.queued == 0


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
def test_the_receive_layout_is_the_layout_of_the_frame_the_receiver_took_last(
    u575: popcorn.PartDescription, running: bool
) -> None:
    m = sai_machine(u575)
    receiver = m.periph.SAI1.audio("B")
    saved = m.snapshot()
    m.reg.pc = m.sym("duplex_irq") & ~1
    before, after = m.time + us(1), m.time + ms(1)

    async def free() -> list[AudioLayout | None]:
        samples = [m.sample(lambda: receiver.receive_layout, at=at) for at in (before, after)]
        async with m.running():
            return [await sample for sample in samples]

    async def parked() -> list[AudioLayout | None]:
        seen: list[AudioLayout | None] = []
        for at in (before, after):
            await m.run(for_=at - m.time)
            seen.append(receiver.receive_layout)
        return seen

    assert receiver.receive_layout is None
    seen = run_bounded(free(), 10) if running else asyncio.run(parked())
    assert seen == [None, I2S_LAYOUT]
    assert receiver.receive_layout == I2S_LAYOUT
    m.restore(saved)
    assert receiver.receive_layout is None


def test_decimation_stretches_every_frame(u575: popcorn.PartDescription) -> None:
    m = sai_machine(u575, {"A": AudioOptions(decimation=2)})
    run_entry(m, "tx_poll")
    frames = asyncio.run(m.periph.SAI1.audio("A").read_frames(FRAMES))
    assert all(later.at - earlier.at == 2 * FRAME for earlier, later in itertools.pairwise(frames))


def test_a_restore_drops_unread_frames_and_unserved_words(
    u575: popcorn.PartDescription,
) -> None:
    m = sai_machine(u575)
    saved = m.snapshot()
    sender, receiver = m.periph.SAI1.audio("A"), m.periph.SAI1.audio("B")
    run_entry(m, "tx_poll")
    asyncio.run(receiver.write([1, 2, 3]))
    assert (sender.available, receiver.queued) == (FRAMES, 3)

    m.restore(saved)
    assert (sender.available, sender.dropped, receiver.queued) == (0, 0, 0)
    run_entry(m, "tx_poll")
    assert read_all(sender)[0] == (ramp(0), ramp(1))


def test_a_full_queue_drops_its_oldest_frames_and_counts_them(
    u575: popcorn.PartDescription,
) -> None:
    m = sai_machine(u575)
    saved = m.snapshot()
    stream = m.periph.SAI1.audio("A")
    link = m._audio._links["SAI1", "A"]  # pyright: ignore[reportPrivateUsage]
    link.set_frame_limit(3)

    run_entry(m, "tx_poll")
    assert stream.dropped == FRAMES - 3
    assert read_all(stream) == [
        (ramp(2 * index), ramp(2 * index + 1)) for index in range(FRAMES - 3, FRAMES)
    ]
    m.restore(saved)
    assert stream.dropped == 0
    with pytest.raises(popcorn.ConfigError):
        link.set_frame_limit(0)


def test_a_secure_alias_shares_the_stream_and_names_the_sai(
    u575: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(SAI_ELF),
        audio={"SEC_SAI1": {"A": AudioOptions(decimation=2)}},
    )
    assert m.periph.SEC_SAI1.audio("A") is m.periph.SAI1.audio("A")
    at_main(m)
    run_entry(m, "tx_poll")
    frames = asyncio.run(m.periph.SEC_SAI1.audio("A").read_frames(2))
    assert frames[1].at - frames[0].at == 2 * FRAME

    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Machine(u575, audio={"SAI1": {}, "SEC_SAI1": {}})
    assert caught.value.key == "audio.SEC_SAI1"


@pytest.mark.parametrize(
    ("audio", "key"),
    [
        ({"USART1": {"A": AudioOptions()}}, "audio.USART1"),
        ({"SAI1": {"A": AudioOptions(decimation=0)}}, "audio.SAI1.A.decimation"),
        ({"SAI1": {"B": AudioOptions(external_frame_hz=-1)}}, "audio.SAI1.B.external_frame_hz"),
        ({"SAI1": {"A": AudioOptions(decimation=1 << 40)}}, "audio.SAI1.A.decimation"),
        ({"SAI1": {"A": AudioOptions(decimation=cast(int, 2.0))}}, "audio.SAI1.A.decimation"),
        ({"SAI1": {"B": AudioOptions(external_frame_hz=True)}}, "audio.SAI1.B.external_frame_hz"),
        ({"SAI1": {"C": AudioOptions()}}, "audio.SAI1.C"),
    ],
)
def test_audio_options_are_checked(
    u575: popcorn.PartDescription, audio: dict[str, dict[Block, AudioOptions]], key: str
) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Machine(u575, audio=audio)
    assert caught.value.key == key


def test_the_accessor_refuses_other_blocks_and_peripherals(
    u575: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(u575, models={"SAI2": Silent})
    with pytest.raises(popcorn.ConfigError):
        m.periph.SAI1.audio(cast(Block, "C"))
    with pytest.raises(popcorn.ModelError):
        m.periph.USART1.audio("A")
    with pytest.raises(popcorn.ModelError):
        m.periph.SAI2.audio("A")
    with pytest.raises(ValueError, match=r"2\*\*32"):
        asyncio.run(m.periph.SAI1.audio("B").write([1 << 32]))


def test_a_write_from_another_thread_during_a_run_is_posted(
    u575: popcorn.PartDescription,
) -> None:
    m = sai_machine(u575)
    receiver = m.periph.SAI1.audio("B")

    async def write() -> VirtualTime:
        return await receiver.write([1])

    async def body() -> VirtualTime:
        async with m.running():
            return await asyncio.to_thread(asyncio.run, write())

    assert asyncio.run(body()) >= VirtualTime(0)


def project(board: Mapping[str, popcorn.Periph]) -> popcorn.MachineDescription:
    return popcorn.MachineDescription(
        part="STM32U575QGIx", images=[popcorn.Elf(SAI_ELF)], board=board
    )


def test_a_project_sets_the_options_of_a_block() -> None:
    options = AudioOptions(decimation=2)
    m = project({"SAI1": popcorn.Periph(audio={"A": options})}).machine()
    at_main(m)
    run_entry(m, "tx_poll")
    frames = asyncio.run(m.periph.SAI1.audio("A").read_frames(2))
    assert frames[1].at - frames[0].at == 2 * FRAME


@pytest.mark.parametrize(
    ("board", "key"),
    [
        (
            {"USART1": popcorn.Periph(audio={"A": AudioOptions(decimation=2)})},
            "board.USART1.A",
        ),
        (
            {
                "SAI1": popcorn.Periph(
                    model=popcorn.Use(Silent), audio={"B": AudioOptions(decimation=2)}
                )
            },
            "board.SAI1.B",
        ),
    ],
)
def test_a_block_table_needs_an_sai_with_the_built_in_model(
    board: dict[str, popcorn.Periph], key: str
) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        project(board).machine()
    assert caught.value.key == key


def codec_dma_machine(u575: popcorn.PartDescription) -> tuple[popcorn.Machine, AudioStream]:
    m = sai_machine(u575)
    m.reg.pc = m.sym("codec_dma")
    stop = asyncio.run(m.run(budget=BUDGET, stop_when=lambda m: m.periph.SAI1.BCR1.SAIAEN == 1))
    assert stop.reason is StopReason.USER, stop
    stop = asyncio.run(m.run(for_=ms(1), budget=BUDGET))
    assert stop.reason is StopReason.TIME, stop
    assert m.periph.SAI1.ACR1.SAIAEN == 1
    return m, m.periph.SAI1.audio("A")


def test_the_external_clock_starts_changes_and_stops_a_slave_block_from_the_test_body(
    u575: popcorn.PartDescription,
) -> None:
    m, sender = codec_dma_machine(u575)
    assert (sender.available, sender.external_frame_hz) == (0, 0)

    t0 = m.time
    sender.external_frame_hz = 16_000
    assert sender.external_frame_hz == 16_000
    asyncio.run(m.run(for_=ms(1)))
    first = asyncio.run(sender.read_frames(sender.available))
    assert [frame.at - t0 for frame in first] == [k * SLAVE_FRAME for k in range(16)]

    sender.external_frame_hz = 8_000
    asyncio.run(m.run(for_=ms(1)))
    slower = asyncio.run(sender.read_frames(sender.available))
    assert [frame.at - t0 for frame in slower] == [
        16 * SLAVE_FRAME,
        *(17 * SLAVE_FRAME + k * 2 * SLAVE_FRAME for k in range(7)),
    ]

    stopped = m.time
    sender.external_frame_hz = 0
    asyncio.run(m.run(for_=ms(2)))
    last = asyncio.run(sender.read_frames(sender.available))
    assert [frame.at - t0 for frame in last] == [31 * SLAVE_FRAME]
    assert all(frame.at < stopped for frame in last)
    assert sender.external_frame_hz == 0


def test_the_external_clock_is_board_state_restored_by_a_snapshot(
    u575: popcorn.PartDescription,
) -> None:
    m, sender = codec_dma_machine(u575)
    stopped = m.snapshot()
    t0 = m.time
    sender.external_frame_hz = 16_000
    asyncio.run(m.run(for_=us(100)))
    running = m.snapshot()

    m.restore(stopped)
    assert sender.external_frame_hz == 0
    asyncio.run(m.run(for_=ms(1)))
    assert sender.available == 0

    other = popcorn.Machine(u575, image=popcorn.Image.elf(SAI_ELF))
    other.restore(popcorn.Snapshot.from_bytes(running.to_bytes()))
    again = other.periph.SAI1.audio("A")
    assert again.external_frame_hz == 16_000
    asyncio.run(other.run(for_=ms(1)))
    frames = asyncio.run(again.read_frames(again.available))
    assert [frame.at - t0 for frame in frames] == [k * SLAVE_FRAME for k in range(1, 17)]


@pytest.mark.parametrize("hz", [-1, True, 1 << 32, cast(int, 16_000.0)])
def test_an_external_frame_rate_outside_32_bits_is_a_value_error(
    u575: popcorn.PartDescription, hz: int
) -> None:
    sender = popcorn.Machine(u575).periph.SAI1.audio("A")
    with pytest.raises(ValueError, match=r"2\*\*32"):
        sender.external_frame_hz = hz
    assert sender.external_frame_hz == 0


def test_an_external_clock_set_from_another_thread_during_a_run_is_refused(
    u575: popcorn.PartDescription,
) -> None:
    m = sai_machine(u575)
    sender = m.periph.SAI1.audio("A")

    def start() -> None:
        sender.external_frame_hz = 16_000

    async def body() -> None:
        async with m.running():
            await asyncio.to_thread(start)

    with pytest.raises(popcorn.MachineRunning, match=r"await m\.read"):
        asyncio.run(body())
    assert sender.external_frame_hz == 0

    async def posted() -> None:
        async with m.running():
            await m.write((m.ref.periph.SAI1.audio("A").external_frame_hz, 16_000))

    asyncio.run(posted())
    assert sender.external_frame_hz == 16_000


class CodecStandIn(I2CTarget):
    """An audio codec on I2C whose register 0x0F, written 1 or 0, starts or stops the frame
    clock it drives into SAI1 block A at 16 kHz, the clock master of the board."""

    register: int
    selected: bool
    started_ps: int
    stopped_ps: int

    def __init__(self, address: int = 0x38) -> None:
        super().__init__(address)
        self.register = 0
        self.selected = False
        self.started_ps = 0
        self.stopped_ps = 0

    def on_write(self, byte: int, at: VirtualTime) -> bool:
        if not self.selected:
            self.register, self.selected = byte, True
        elif self.register == 0x0F:
            self.board.set_audio_clock("SAI1", "A", 16_000 if byte & 1 else 0)
            if byte & 1:
                self.started_ps = at.ps
            else:
                self.stopped_ps = at.ps
        return True

    def on_stop(self, at: VirtualTime) -> None:
        self.selected = False


def codec_words() -> list[int]:
    return [word for k in range(CODEC_FRAMES) for word in (0x1000 | k, 0x2000 | k)]


def codec_machine(
    u575: popcorn.PartDescription, codec: CodecStandIn, words: list[int]
) -> popcorn.Machine:
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(SAI_ELF),
        devices={"I2C1": [codec]},
        audio={"SAI1": {"A": AudioOptions(clocked_by="I2C1:0x38")}},
    )
    at_main(m)
    m.reg.pc = m.sym("codec_i2c")
    asyncio.run(m.periph.SAI1.audio("B").write(words))
    return m


def halfwords(m: popcorn.Machine, name: str) -> list[list[int]]:
    raw = m.mem.read(m.sym(name), 4 * STEREO_FRAMES)
    flat = struct.unpack(f"<{2 * STEREO_FRAMES}H", raw)
    return [list(flat[:STEREO_FRAMES]), list(flat[STEREO_FRAMES:])]


def event_ticks(m: popcorn.Machine, name: str) -> list[int]:
    count = m.mem.u32[m.sym(name) + 8]
    return [m.mem.u32[m.sym(name) + 12 + 4 * index] for index in range(count)]


def test_a_python_codec_on_i2c_starts_and_stops_the_sai1_clock_of_the_audio_path(
    u575: popcorn.PartDescription,
) -> None:
    codec = CodecStandIn()
    m = codec_machine(u575, codec, codec_words())
    stop = asyncio.run(m.run(budget=1_000_000_000))
    assert stop.reason is StopReason.USER, stop
    t0 = VirtualTime(codec.started_ps)
    t_stop = VirtualTime(codec.stopped_ps)
    assert t0 < t_stop

    sent = asyncio.run(m.periph.SAI1.audio("A").read_frames(m.periph.SAI1.audio("A").available))
    count = len(sent)
    assert count > 1280
    assert VirtualDuration(0) <= sent[0].at - t0 <= us(1)
    t0 = sent[0].at
    assert [frame.at - t0 for frame in sent] == [k * SLAVE_FRAME for k in range(count)]
    assert [frame.slots for frame in sent] == [
        (k % STEREO_FRAMES, 0x8000 | k % STEREO_FRAMES) for k in range(count)
    ]
    assert sent[-1].at < t_stop <= sent[-1].at + SLAVE_FRAME

    tx, rx = halfwords(m, "tx_stereo"), halfwords(m, "rx_stereo")
    last = [max(f for f in range(count) if f % STEREO_FRAMES == k) for k in range(STEREO_FRAMES)]
    assert rx == [[0x1000 | j for j in last], [0x2000 | j for j in last]]
    assert m.periph.SAI1.ASR.OVRUDR == 0
    assert m.periph.SAI1.BSR.OVRUDR == 0

    sums = log(m)
    assert sums[-2:] == [checksum(tx), checksum(rx)]
    halves = 0x00480000
    completes = 0x00540000
    events = [halves | 1, completes | 1, halves | 2, completes | 2, halves | 3]
    assert [word for word in sums[:-2] if word >> 24 == 0x0D][:5] == [
        0x0D000000 | e for e in events
    ]
    assert [word for word in sums[:-2] if word >> 24 == 0x0C][:5] == [
        0x0C000000 | e for e in events
    ]

    epoch = m.time - us(m.mem.u32[TIM2_CNT])
    due_tx = [t0 + (block * SLAVE_FRAME) for block in (255, 511, 767, 1023, 1279)]
    due_rx = [t0 + block * SLAVE_FRAME + us(46.875) for block in (255, 511, 767, 1023, 1279)]
    late = [
        seen - (due - epoch).ps // 1_000_000
        for name, dues in (("stereo_tx_events", due_tx), ("stereo_rx_events", due_rx))
        for seen, due in zip(event_ticks(m, name), dues, strict=True)
    ]
    assert min(late) >= 0
    assert max(late) <= 5
    assert max(late) - min(late) <= 1


def checksum(buffer: list[list[int]]) -> int:
    total = 0
    for k in range(STEREO_FRAMES):
        total = (total * 31 + buffer[0][k]) & 0xFFFFFFFF
        total = (total * 31 + buffer[1][k]) & 0xFFFFFFFF
    return total


def test_a_codec_run_restored_mid_stream_into_a_fresh_machine_and_codec_runs_the_same(
    u575: popcorn.PartDescription,
) -> None:
    words = codec_words()
    codec = CodecStandIn()
    m = codec_machine(u575, codec, words)
    asyncio.run(m.run(budget=1_000_000_000, stop_when=lambda _: codec.started_ps != 0))
    t0 = VirtualTime(codec.started_ps)
    asyncio.run(m.run(for_=t0 + ms(20) - m.time))
    sender, receiver = m.periph.SAI1.audio("A"), m.periph.SAI1.audio("B")
    asyncio.run(sender.read_frames(sender.available))
    unserved = words[len(words) - receiver.queued :]
    saved = m.snapshot()
    stop = asyncio.run(m.run(budget=1_000_000_000))
    assert stop.reason is StopReason.USER, stop
    rest = asyncio.run(sender.read_frames(sender.available))

    fresh = CodecStandIn()
    other = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(SAI_ELF),
        devices={"I2C1": [fresh]},
        audio={"SAI1": {"A": AudioOptions(clocked_by="I2C1:0x38")}},
    )
    other.restore(popcorn.Snapshot.from_bytes(saved.to_bytes()))
    assert fresh.started_ps == codec.started_ps
    asyncio.run(other.periph.SAI1.audio("B").write(unserved))
    stop = asyncio.run(other.run(budget=1_000_000_000))
    assert stop.reason is StopReason.USER, stop
    again = other.periph.SAI1.audio("A")
    frames: list[AudioFrame] = asyncio.run(again.read_frames(again.available))
    assert frames == rest
    assert fresh.stopped_ps == codec.stopped_ps
    assert log(other) == log(m)
    for name in ("tx_stereo", "rx_stereo"):
        assert halfwords(other, name) == halfwords(m, name)
    for name in ("stereo_tx_events", "stereo_rx_events"):
        assert event_ticks(other, name) == event_ticks(m, name)


def test_a_project_wires_the_codec_clock_as_audio_options_do() -> None:
    board = {
        "I2C1": popcorn.Periph(devices=[popcorn.Device(CodecStandIn, address=0x38)]),
        "SAI1": popcorn.Periph(audio={"A": AudioOptions(clocked_by="I2C1:0x38")}),
    }
    m = project(board).machine()
    at_main(m)
    m.reg.pc = m.sym("codec_i2c")
    asyncio.run(m.periph.SAI1.audio("B").write(codec_words()))
    stop = asyncio.run(m.run(budget=1_000_000_000))
    assert stop.reason is StopReason.USER, stop
    sender = m.periph.SAI1.audio("A")
    assert sender.available > 1280
    frames = asyncio.run(sender.read_frames(sender.available))
    assert all(
        later.at - earlier.at == SLAVE_FRAME for earlier, later in itertools.pairwise(frames)
    )
    assert sender.external_frame_hz == 0


def unclocked_warnings(caplog: pytest.LogCaptureFixture) -> list[str]:
    return [r.getMessage() for r in caplog.records if "external frame rate" in r.getMessage()]


def test_a_block_wired_to_a_codec_waits_for_its_clock_without_a_warning(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    codec = CodecStandIn()
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        m = codec_machine(u575, codec, codec_words())
        stop = asyncio.run(m.run(budget=BUDGET, stop_when=lambda _: codec.started_ps != 0))
        assert stop.reason is StopReason.USER, stop
    assert m.periph.SAI1.ACR1.SAIAEN == 1
    assert unclocked_warnings(caplog) == []


def test_a_slave_block_nothing_clocks_logs_once_that_it_sees_no_frame(
    u575: popcorn.PartDescription, caplog: pytest.LogCaptureFixture
) -> None:
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        codec_dma_machine(u575)
    assert len(unclocked_warnings(caplog)) == 1


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
def test_a_frame_read_waits_on_the_machine_thread_without_the_gil(
    u575: popcorn.PartDescription, running: bool
) -> None:
    m = sai_machine(u575)
    run_entry(m, "tx_poll")
    m.reg.pc = m.sym("main") & ~1
    stream = m.periph.SAI1.audio("A")

    async def scenario() -> int:
        if running:
            async with m.running():
                return len(await stream.read_frames(FRAMES + 1, for_=ms(1)))
        return len(await stream.read_frames(FRAMES + 1, for_=ms(1)))

    assert asyncio.run(scenario()) >= FRAMES
    assert m._handle.gil_entries() == 0  # pyright: ignore[reportPrivateUsage]


def test_take_frames_equals_a_read_of_what_is_available(
    u575: popcorn.PartDescription,
) -> None:
    m = sai_machine(u575)
    start = m.snapshot()
    stream = m.periph.SAI1.audio("A")
    run_entry(m, "tx_poll")
    taken = stream.take_frames()
    assert len(taken) == FRAMES
    assert (stream.available, stream.take_frames()) == (0, [])
    m.restore(start)
    run_entry(m, "tx_poll")
    assert taken == asyncio.run(stream.read_frames(stream.available))


def test_an_audio_read_with_a_deadline_stops_there(u575: popcorn.PartDescription) -> None:
    m = sai_machine(u575)
    stream = m.periph.SAI1.audio("A")
    before = m.instructions
    assert asyncio.run(stream.read_frames(1, deadline=VirtualTime(0))) == []
    assert m.instructions == before
    with pytest.raises(TypeError, match="exclusive"):
        asyncio.run(stream.read_frames(1, for_=ms(1), deadline=m.time))
    end = m.time + ms(1)
    assert asyncio.run(stream.read_frames(10**6, deadline=end)) is not None
    assert m.time >= end


def test_an_audio_read_with_a_past_deadline_ends_at_once_while_running(
    u575: popcorn.PartDescription,
) -> None:
    m = sai_machine(u575)
    stream = m.periph.SAI1.audio("A")

    async def scenario() -> list[AudioFrame]:
        async with m.running():
            past = await m.now()
            return await stream.read_frames(10**6, deadline=past)

    frames = run_bounded(scenario(), 10)
    assert len(frames) < 10**6


def test_a_free_run_deadline_read_takes_the_frames_a_parked_one_takes(
    u575: popcorn.PartDescription,
) -> None:
    m = sai_machine(u575)
    m.reg.pc = m.sym("tx_poll") & ~1
    asyncio.run(m.run(for_=us(500)))
    m.reg.pc = m.sym("idle") & ~1
    stream = m.periph.SAI1.audio("A")
    stream.take_frames()
    start = m.snapshot()

    async def reads(at: VirtualTime) -> tuple[list[AudioFrame], list[AudioFrame]]:
        first = await stream.read_frames(10**6, deadline=at + ms(2))
        return first, await stream.read_frames(10**6, deadline=at + ms(4))

    async def running() -> tuple[VirtualTime, tuple[list[AudioFrame], list[AudioFrame]]]:
        async with m.running():
            return await late_drained_read(m, lambda _: None, reads, ms(3))

    at, (first, later) = run_bounded(running(), 10)
    assert first
    assert later
    assert first[-1].at + FRAME <= at + ms(2) < later[0].at + FRAME
    frames = first + later
    assert all(b.at - a.at == FRAME for a, b in itertools.pairwise(frames))
    assert all(frame.layout == I2S_LAYOUT for frame in frames)
    m.restore(start)
    assert asyncio.run(reads(at)) == (first, later)
