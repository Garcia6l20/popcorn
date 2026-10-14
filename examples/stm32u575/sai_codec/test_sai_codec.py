"""SAI1 as a slave pair clocked by an I2C codec, two 2D circular GPDMA lists, HAL.

`stereocodec.StereoCodec` is the codec, a `popcorn.board.I2CTarget` on I2C1 at 0x1A. The project
wires its frame clock to SAI1 block A with `AudioOptions(clocked_by="I2C1:0x1A")`, so its
`self.board.set_audio_clock` may start and stop that clock. Block B is synchronous to A and
frames with it.

The firmware streams `playback`, an `int16_t[2][64]` with one row per channel, out of block A and
writes what block B receives into `capture`, the same shape. GPDMA1 channel 12 interleaves the
rows into frames, channel 13 splits frames back into rows, each with one 2D node: a block is
one frame, two halfwords, repeated 64 times. Then the firmware powers the codec on, waits for
four complete callbacks on the receive side and powers it off.

`m.periph.SAI1.audio("A")` holds the frames block A sent. `m.periph.SAI1.audio("B")` takes the
slot words the codec sends to block B, two per frame.
"""

import itertools

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.board import AudioFrame
from popcorn.units import VirtualTime, ms, us
from stereocodec import StereoCodec

pytestmark = pytest.mark.popcorn(budget=2_000_000)

FRAMES = 64
BUFFERS = 4
FRAME_PS = 10**12 / 48_000
RECEIVED = [word for k in range(1024) for word in (0x1000 | k, 0x2000 | k)]


def codec(machine: Machine) -> StereoCodec:
    (device,) = machine.board.devices("I2C1")
    assert isinstance(device, StereoCodec)
    return device


async def stream_until_the_codec_stops(machine: Machine) -> None:
    stop = await machine.run(until="codec_stopped")
    assert stop.reason is StopReason.ADDRESS, stop


def sent(machine: Machine) -> list[AudioFrame]:
    return machine.periph.SAI1.audio("A").take_frames()


def rows(machine: Machine, name: str) -> list[list[int]]:
    buffer = machine.var(name)
    return [[int(buffer[row][k].value) & 0xFFFF for k in range(FRAMES)] for row in range(2)]


async def test_the_codec_frames_block_a_from_its_power_write_to_the_end_of_the_frame_in_progress(
    sai_codec: Machine,
) -> None:
    await sai_codec.periph.SAI1.audio("B").write(RECEIVED)
    await stream_until_the_codec_stops(sai_codec)
    stop = await sai_codec.run(for_=ms(1))
    assert stop.reason is StopReason.TIME, stop
    frames = sent(sai_codec)
    started = VirtualTime(codec(sai_codec).started_ps)
    stopped = VirtualTime(codec(sai_codec).stopped_ps)
    assert frames[0].at == started
    for k, frame in enumerate(frames):
        assert abs((frame.at - started).ps - k * FRAME_PS) < 1
    assert frames[-1].at < stopped < frames[-1].at + us(20.84)
    assert len(frames) > BUFFERS * FRAMES
    assert sai_codec.periph.SAI1.audio("A").external_frame_hz == 0
    assert sai_codec.periph.SAI1.ASR.OVRUDR == 0
    assert sai_codec.periph.SAI1.BSR.OVRUDR == 0


async def test_channel_12_interleaves_the_two_rows_of_playback_into_frames(
    sai_codec: Machine,
) -> None:
    await sai_codec.periph.SAI1.audio("B").write(RECEIVED)
    await stream_until_the_codec_stops(sai_codec)
    left, right = rows(sai_codec, "playback")
    assert (left[1], right[1]) == (512, 0x10000 - 512)
    frames = sent(sai_codec)
    assert [frame.slots for frame in frames] == [
        (left[k % FRAMES], right[k % FRAMES]) for k in range(len(frames))
    ]
    assert all((frame.data_bits, frame.active_slots) == (16, 0b11) for frame in frames)


async def test_channel_13_splits_each_received_frame_into_the_two_rows_of_capture(
    sai_codec: Machine,
) -> None:
    await sai_codec.periph.SAI1.audio("B").write(RECEIVED)
    await stream_until_the_codec_stops(sai_codec)
    frames = sent(sai_codec)
    last = [max(k for k in range(len(frames)) if k % FRAMES == j) for j in range(FRAMES)]
    assert rows(sai_codec, "capture") == [[0x1000 | k for k in last], [0x2000 | k for k in last]]
    assert sai_codec.periph.SAI1.audio("B").receive_layout == frames[0].layout


async def test_half_and_complete_callbacks_come_per_2d_block_of_64_frames(
    sai_codec: Machine,
) -> None:
    seen: dict[str, list[int]] = {}

    def recorder(name: str) -> None:
        def record(call: Call) -> Return:
            seen.setdefault(name, []).append(sai_codec.time.ps)
            return call.proceed()

        sai_codec.intercept(name, record)

    for name in ("tx_half", "tx_complete", "rx_half", "rx_complete"):
        recorder(name)
    await sai_codec.periph.SAI1.audio("B").write(RECEIVED)
    await stream_until_the_codec_stops(sai_codec)
    assert {name: len(times) for name, times in seen.items()} == {
        "tx_half": BUFFERS,
        "tx_complete": BUFFERS,
        "rx_half": BUFFERS,
        "rx_complete": BUFFERS,
    }
    started = codec(sai_codec).started_ps
    for name, frames, earliest, latest in (
        ("rx_half", FRAMES // 2, us(12.66), us(12.84)),
        ("rx_complete", FRAMES, us(16.41), us(16.59)),
    ):
        due = [started + (frames + buffer * FRAMES) * FRAME_PS for buffer in range(BUFFERS)]
        late = [time - when for time, when in zip(seen[name], due, strict=True)]
        assert all(earliest.ps <= delay <= latest.ps for delay in late), (name, late)
    for times in seen.values():
        for earlier, later in itertools.pairwise(times):
            assert abs(later - earlier - FRAMES * FRAME_PS) < us(1).ps


async def test_a_snapshot_mid_stream_replays_the_same_frames_and_codec_stop(
    sai_codec: Machine,
) -> None:
    receiver = sai_codec.periph.SAI1.audio("B")
    await receiver.write(RECEIVED)
    stop = await sai_codec.run(stop_when=lambda m: codec(m).started_ps != 0)
    assert stop.reason is StopReason.USER, stop
    stop = await sai_codec.run(for_=ms(2))
    assert stop.reason is StopReason.TIME, stop
    sent(sai_codec)
    unserved = RECEIVED[len(RECEIVED) - receiver.queued :]
    saved = sai_codec.snapshot()
    await stream_until_the_codec_stops(sai_codec)
    rest = sent(sai_codec)
    captured = rows(sai_codec, "capture")
    stopped = codec(sai_codec).stopped_ps

    sai_codec.restore(saved)
    assert codec(sai_codec).stopped_ps == 0
    await receiver.write(unserved)
    await stream_until_the_codec_stops(sai_codec)
    assert sent(sai_codec) == rest
    assert rows(sai_codec, "capture") == captured
    assert codec(sai_codec).stopped_ps == stopped
