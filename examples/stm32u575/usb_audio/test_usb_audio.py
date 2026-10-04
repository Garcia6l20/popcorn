"""A TinyUSB 0.21.0 USB Audio 2.0 headset, probed and streamed by popcorn's virtual host.

`[board.OTG_FS] host = {}` in `popcorn.toml` plugs a virtual full-speed host into the OTG_FS
model from power-on. It enumerates the device, then its audio driver probes the function as
Linux snd-usb-audio does: the clock's sample rate range, current rate and validity for each
streaming setting, then the mute and volume of each feature unit.

`machine.periph.OTG_FS.host.audio()` is the function. `audio.speaker` sends PCM on the
isochronous OUT endpoint, `audio.mic` receives it on the isochronous IN endpoint, one packet per
1 ms frame, 16 samples at 16 kHz. The firmware copies what the speaker receives to the
microphone, so what a test writes comes back on the mic.
"""

import itertools
import math

import pytest
from popcorn import Machine, UsbError
from popcorn.board import UacFormat, UacFunction, UsbHost, UsbRequest
from popcorn.units import ms, s

pytestmark = pytest.mark.popcorn(budget=5_000_000)

AC, SPEAKER, MIC = 0, 1, 2
CLOCK, SPEAKER_UNIT, MIC_UNIT = 0x04, 0x02, 0x12
GET_CUR, GET_RANGE, SET_CUR = (0xA1, 0x01), (0xA1, 0x02), (0x21, 0x01)
SET_INTERFACE = (0x01, 0x0B)
SAMPLE_RATE, CLOCK_VALID = 0x0100, 0x0200
MUTE, VOLUME = 0x0100, 0x0200
SAMPLES_PER_FRAME = 16
SINE = [round(12000 * math.sin(2 * math.pi * 1000 * n / 16000)) for n in range(16 * 160)]

type Setup = tuple[int, int, int, int, int]


def setup(kind: tuple[int, int], value: int, index: int, length: int) -> Setup:
    return (*kind, value, index, length)


def clock(kind: tuple[int, int], selector: int, length: int) -> Setup:
    return setup(kind, selector, CLOCK << 8 | AC, length)


def unit(kind: tuple[int, int], unit_id: int, control: int, channel: int, length: int) -> Setup:
    return setup(kind, control | channel, unit_id << 8 | AC, length)


def streaming_probe(interface: int) -> list[Setup]:
    return [
        clock(GET_RANGE, SAMPLE_RATE, 2),
        clock(GET_RANGE, SAMPLE_RATE, 14),
        setup(SET_INTERFACE, 0, interface, 0),
        clock(GET_CUR, CLOCK_VALID, 1),
        clock(GET_CUR, SAMPLE_RATE, 4),
        clock(GET_CUR, CLOCK_VALID, 1),
        setup(SET_INTERFACE, 1, interface, 0),
        setup(SET_INTERFACE, 0, interface, 0),
    ]


def volume_probe(unit_id: int, channel: int) -> list[Setup]:
    return [
        *[unit(GET_RANGE, unit_id, VOLUME, channel, 8)] * 3,
        unit(GET_CUR, unit_id, VOLUME, channel, 2),
        unit(SET_CUR, unit_id, VOLUME, channel, 2),
        unit(GET_CUR, unit_id, VOLUME, channel, 2),
        unit(SET_CUR, unit_id, VOLUME, channel, 2),
    ]


def feature_probe(unit_id: int) -> list[Setup]:
    return [
        unit(GET_CUR, unit_id, MUTE, 1, 1),
        unit(GET_CUR, unit_id, MUTE, 0, 1),
        *volume_probe(unit_id, 1),
        *volume_probe(unit_id, 0),
    ]


def setups(requests: tuple[UsbRequest, ...]) -> list[Setup]:
    return [
        (r.request_type, r.request, r.value, r.index, r.length)
        for r in requests
        if r.request_type & 0x60 or r.request == SET_INTERFACE[1]
    ]


async def ready_audio(machine: Machine) -> tuple[UsbHost, UacFunction]:
    host = machine.periph.OTG_FS.host
    audio = host.audio()
    await host.wait_configured(for_=s(2))
    await audio.wait_ready(for_=ms(200))
    return host, audio


async def streaming(machine: Machine) -> UacFunction:
    """The function with both streams running and the speaker's first packet gone. TinyUSB
    0.21.0 arms its isochronous OUT endpoint for the frame after the one it handles
    SET_INTERFACE in, and does not handle an incomplete isochronous OUT. The virtual host ends
    that request in the frame of its SETUP and, as Linux on xHCI, sends the first packet two
    frames later, in the other parity, so the model always loses it. A real host loses 0 or 1
    packet, depending on its isochronous scheduling threshold and when it submits. PCM written
    after the first frames is never lost."""
    _, audio = await ready_audio(machine)
    await audio.mic.start()
    await audio.speaker.start()
    await machine.run(for_=ms(4))
    audio.mic.clear()
    return audio


async def test_the_host_probes_the_function_as_linux_snd_usb_audio(machine: Machine) -> None:
    host = machine.periph.OTG_FS.host
    earlier = len(host.requests)
    await ready_audio(machine)
    assert setups(host.requests[earlier:]) == [
        *streaming_probe(SPEAKER),
        *streaming_probe(MIC),
        *feature_probe(SPEAKER_UNIT),
        *feature_probe(MIC_UNIT),
    ]
    assert all(r.status == "completed" for r in host.requests[earlier:])
    assert machine.var("clock_set_requests").value == 0


async def test_the_streams_take_their_format_and_rate_from_the_descriptors(
    machine: Machine,
) -> None:
    _, audio = await ready_audio(machine)
    for stream, interface in ((audio.speaker, SPEAKER), (audio.mic, MIC)):
        assert stream.bound
        assert (stream.interface, stream.alternate) == (interface, 1)
        assert stream.format == UacFormat(channels=1, bytes_per_sample=2, bits=16)
        assert stream.sample_rates == (16000,)
        assert stream.rate == 16000
        assert not stream.running


async def test_the_feature_units_hold_what_the_probe_read(machine: Machine) -> None:
    _, audio = await ready_audio(machine)
    units = {u.unit: u for u in audio.feature_units}
    assert sorted(units) == [SPEAKER_UNIT, MIC_UNIT]
    for found in units.values():
        assert found.channels == 1
        assert found.mute == (False, False)
        assert found.volume == (0, 0)
        assert found.volume_range == ((-50 * 256, 0, 256),) * 2


async def test_set_volume_and_mute_reach_the_firmware(machine: Machine) -> None:
    _, audio = await ready_audio(machine)
    await audio.set_volume(SPEAKER_UNIT, 1, -10 * 256)
    await audio.set_mute(MIC_UNIT, 0, True)
    assert machine.var("feature_volume")[0][1].value == -10 * 256
    assert machine.var("feature_mute")[1][0].value == 1
    units = {u.unit: u for u in audio.feature_units}
    assert units[SPEAKER_UNIT].volume == (0, -10 * 256)
    assert units[MIC_UNIT].mute == (True, False)
    with pytest.raises(ValueError, match="no such unit"):
        await audio.set_volume(0x33, 0, 0)


async def test_start_selects_the_alternate_setting_after_reading_the_clock(
    machine: Machine,
) -> None:
    host, audio = await ready_audio(machine)
    earlier = len(host.requests)
    await audio.mic.start()
    await audio.speaker.start()
    assert setups(host.requests[earlier:]) == [
        clock(GET_CUR, CLOCK_VALID, 1),
        clock(GET_CUR, SAMPLE_RATE, 4),
        clock(GET_CUR, CLOCK_VALID, 1),
        setup(SET_INTERFACE, 1, MIC, 0),
        setup(SET_INTERFACE, 1, SPEAKER, 0),
    ]
    assert audio.speaker.running and audio.mic.running
    assert (machine.var("speaker_alt").value, machine.var("mic_alt").value) == (1, 1)


async def test_a_sine_written_to_the_speaker_comes_back_on_the_mic(machine: Machine) -> None:
    audio = await streaming(machine)
    await audio.speaker.write(SINE)
    frames = await audio.mic.read_frames(170, for_=ms(200))
    assert len(frames) == 170
    assert all(b.at - a.at == ms(1) for a, b in itertools.pairwise(frames))
    received = [sample for frame in frames for sample in frame.samples]
    start = next(n for n, sample in enumerate(received) if sample) - 1
    assert received[:start] == [0] * start
    assert received[start : start + len(SINE)] == SINE
    first = next(n for n, frame in enumerate(frames) if any(frame.samples))
    assert first == 2
    assert all(len(frame.samples) in (15, 16) for frame in frames)


async def test_the_mic_hears_silence_while_nothing_is_written(machine: Machine) -> None:
    audio = await streaming(machine)
    frames = await audio.mic.read_frames(20, for_=ms(30))
    assert len(frames) == 20
    assert all(frame.samples for frame in frames)
    assert all(sample == 0 for frame in frames for sample in frame.samples)


async def test_drain_returns_once_the_last_sample_left_in_a_packet(machine: Machine) -> None:
    audio = await streaming(machine)
    await audio.speaker.write(SINE[: 10 * SAMPLES_PER_FRAME + 5])
    assert audio.speaker.queued == 10 * SAMPLES_PER_FRAME + 5
    await audio.speaker.drain(for_=ms(50))
    assert audio.speaker.queued == 0
    sent = audio.speaker.sent_at
    assert sent is not None
    assert machine.time - sent <= ms(1)


async def test_stop_closes_the_stream_and_no_packet_follows(machine: Machine) -> None:
    host = machine.periph.OTG_FS.host
    audio = await streaming(machine)
    earlier = len(host.requests)
    audio.speaker.stop()
    audio.mic.stop()
    await machine.run(for_=ms(10))
    assert not audio.speaker.running and not audio.mic.running
    assert (machine.var("speaker_alt").value, machine.var("mic_alt").value) == (0, 0)
    assert setups(host.requests[earlier:]) == [
        setup(SET_INTERFACE, 0, SPEAKER, 0),
        setup(SET_INTERFACE, 0, MIC, 0),
    ]
    audio.mic.clear()
    heard = machine.var("speaker_bytes").value
    await machine.run(for_=ms(10))
    assert audio.mic.available == 0
    assert machine.var("speaker_bytes").value == heard


async def test_stop_from_sync_code_with_the_machine_stopped(machine: Machine) -> None:
    host = machine.periph.OTG_FS.host
    audio = await streaming(machine)
    await audio.speaker.write(SINE)
    sent = audio.speaker.sent_at

    def teardown() -> None:
        audio.speaker.stop()
        audio.mic.stop()

    earlier = len(host.requests)
    stopped_at = machine.time
    teardown()
    assert not audio.speaker.running and not audio.mic.running
    assert audio.speaker.queued == 0
    assert host.requests[earlier:] == ()
    audio.mic.clear()
    await machine.run(for_=ms(10))
    assert audio.mic.available == 0
    assert audio.speaker.sent_at == sent
    requests = host.requests[earlier:]
    assert setups(requests) == [
        setup(SET_INTERFACE, 0, SPEAKER, 0),
        setup(SET_INTERFACE, 0, MIC, 0),
    ]
    assert all(r.at > stopped_at for r in requests)


async def test_clear_drops_the_frames_read_before_it(machine: Machine) -> None:
    audio = await streaming(machine)
    await machine.run(for_=ms(5))
    assert audio.mic.available >= 4
    cleared_at = machine.time
    audio.mic.clear()
    assert audio.mic.available == 0
    frames = await audio.mic.read_frames(1, for_=ms(5))
    assert frames[0].at > cleared_at


async def test_a_restore_mid_stream_resumes_the_speaker_queue(machine: Machine) -> None:
    audio = await streaming(machine)
    await audio.speaker.write(SINE)
    await machine.run(for_=ms(20))
    queued = audio.speaker.queued
    assert 0 < queued < len(SINE)
    middle = machine.snapshot()
    await machine.run(for_=ms(30))
    assert audio.speaker.queued == queued - 30 * SAMPLES_PER_FRAME

    machine.restore(middle)
    assert audio.mic.available == 0
    assert audio.speaker.queued == queued
    assert audio.speaker.running and audio.mic.running
    rest = SINE[len(SINE) - queued :]
    frames = await audio.mic.read_frames(queued // SAMPLES_PER_FRAME + 10, for_=ms(200))
    heard = [sample for frame in frames for sample in frame.samples]
    starts = [n for n in range(len(heard) - len(rest) + 1) if heard[n : n + len(rest)] == rest]
    assert starts


async def test_a_start_without_a_bound_function_raises(machine: Machine) -> None:
    host = machine.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    await host.detach()
    await machine.run(for_=ms(5))
    with pytest.raises(UsbError, match="unbound"):
        await host.audio().speaker.start()
