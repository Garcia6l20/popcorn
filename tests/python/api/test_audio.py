"""SAI audio streams: frames a block transmits, captured from Python in virtual time.

The LL test firmware's SAI1 block A is a 16-bit stereo master whose frame lasts 64 us, the
native SAI test's frame time. A frame's `at` is its start."""

from __future__ import annotations

import asyncio
import itertools
import struct
from pathlib import Path
from typing import Literal, cast

import popcorn
import pytest
from popcorn import StopReason
from popcorn.board import AudioOptions, AudioStream
from popcorn.model import Model
from popcorn.units import ms, us

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
SAI_ELF = PREBUILT / "sai_ll.elf"
BUDGET = 100_000_000
FRAMES = 64
FRAME = us(64)

type Block = Literal["A", "B"]


class Silent(Model):
    """A Python model with no register behaviour, standing in for an SAI."""


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
    u575: popcorn.MachineDescription, options: dict[Block, AudioOptions] | None = None
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
    u575: popcorn.MachineDescription,
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
    assert (stream.available, stream.dropped) == (0, 0)


def test_a_receiving_block_takes_the_words_written_in_slot_order(
    u575: popcorn.MachineDescription,
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


def test_decimation_stretches_every_frame(u575: popcorn.MachineDescription) -> None:
    m = sai_machine(u575, {"A": AudioOptions(decimation=2)})
    run_entry(m, "tx_poll")
    frames = asyncio.run(m.periph.SAI1.audio("A").read_frames(FRAMES))
    assert all(later.at - earlier.at == 2 * FRAME for earlier, later in itertools.pairwise(frames))


def test_a_restore_drops_unread_frames_and_unserved_words(
    u575: popcorn.MachineDescription,
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
    u575: popcorn.MachineDescription,
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
    u575: popcorn.MachineDescription,
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
    u575: popcorn.MachineDescription, audio: dict[str, dict[Block, AudioOptions]], key: str
) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Machine(u575, audio=audio)
    assert caught.value.key == key


def test_the_accessor_refuses_other_blocks_and_peripherals(
    u575: popcorn.MachineDescription,
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


def test_a_write_from_another_thread_during_a_run_is_refused(
    u575: popcorn.MachineDescription,
) -> None:
    m = sai_machine(u575)
    receiver = m.periph.SAI1.audio("B")

    async def body() -> None:
        async with m.running():
            await asyncio.to_thread(asyncio.run, receiver.write([1]))

    with pytest.raises(RuntimeError, match="another thread"):
        asyncio.run(body())
    assert receiver.queued == 0


def project(tmp_path: Path, board: str) -> popcorn.Project:
    path = tmp_path / "popcorn.toml"
    path.write_text(f'[mcu]\npart = "STM32U575QGIx"\n[image.app]\nelf = "{SAI_ELF}"\n{board}')
    return popcorn.Project.load(path)


def test_a_project_file_sets_the_options_of_a_block(tmp_path: Path) -> None:
    m = project(tmp_path, "[board.SAI1.A]\ndecimation = 2\n").machine()
    at_main(m)
    run_entry(m, "tx_poll")
    frames = asyncio.run(m.periph.SAI1.audio("A").read_frames(2))
    assert frames[1].at - frames[0].at == 2 * FRAME


@pytest.mark.parametrize(
    ("board", "key"),
    [
        ("[board.USART1.A]\ndecimation = 2\n", "board.USART1.A"),
        (
            f'[models]\npython = ["{Path(__file__).parent}"]\n'
            '[board.SAI1]\nmodel = "test_audio:Silent"\n[board.SAI1.B]\ndecimation = 2\n',
            "board.SAI1.B",
        ),
    ],
)
def test_a_block_table_needs_an_sai_with_the_built_in_model(
    tmp_path: Path, board: str, key: str
) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        project(tmp_path, board).machine()
    assert caught.value.key == key
