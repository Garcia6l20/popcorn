"""A native plugin's RNG model and SPI device, `plugin/rng.cpp` and `plugin/adc.cpp`.

The firmware first reads eight words from the RNG, each by `HAL_RNG_GenerateRandomNumber_IT`,
stored from `HAL_RNG_ReadyDataCallback`, then converts both channels of the SPI ADC on PB0, 16
rounds with `HAL_Delay(1)` between them. The plugin's `rng` model behaves as the python_model
example's `rngmodel.Rng` with seed 1, its ADC as the spi_mock example's `spiadc.Adc` with its
default ramp, so the tests check the same words, timing, samples and framing.
"""

import itertools
from collections.abc import Callable, Iterator
from typing import Protocol, cast, runtime_checkable

import pytest
from popcorn import (
    Call,
    ConfigError,
    Machine,
    MachineDescription,
    ModelError,
    Return,
    Snapshot,
    StopReason,
)
from popcorn.board import AudioOptions, SpiTarget
from popcorn.devices import SpiTargetBase
from popcorn.units import VirtualTime, ms, us

BUDGET = 2_000_000
pytestmark = pytest.mark.popcorn(budget=BUDGET)

COUNT = 8
CONVERSION = us(40)
RNG_NSCR_NIST_VALUE = 0x17CBB
CONVERSIONS = 16
CHANNELS = 2
FULL_SCALE = 0xFFF
CONDRST_BIT = 30
RNGEN_BIT = 2
BSON_FIXED_SIZES = {0x01: 8, 0x08: 1, 0x09: 8, 0x0A: 0, 0x10: 4, 0x11: 8, 0x12: 8}


@runtime_checkable
class Framing(Protocol):
    """The counters `conftest.Adc` takes from the plugin's `AdcBase`."""

    @property
    def counts_per_ms(self) -> int: ...
    @property
    def selects(self) -> int: ...
    @property
    def deselects(self) -> int: ...
    @property
    def bad_frames(self) -> int: ...


def words(seed: int) -> Iterator[int]:
    """The xorshift32 sequence after `seed`, as `plugin/rng.cpp` produces it."""
    state = seed
    while True:
        state ^= state << 13 & 0xFFFFFFFF
        state ^= state >> 17
        state ^= state << 5 & 0xFFFFFFFF
        yield state


async def run_to_done(machine: Machine) -> None:
    stop = await machine.run(until="rng_done", for_=ms(20))
    assert stop.reason is StopReason.ADDRESS, stop


async def run_to_adc_done(machine: Machine) -> None:
    stop = await machine.run(until="adc_done", for_=ms(50))
    assert stop.reason is StopReason.ADDRESS, stop


def framing(device: Framing) -> tuple[int, int, int]:
    return device.selects, device.deselects, device.bad_frames


def short_model_state(saved: bytes, owner: str) -> bytes:
    """`saved` with the last byte of the model state of `owner` dropped, as a corrupt file holds
    it, every enclosing BSON length adjusted so that only the model sees the damage."""
    data = bytearray(saved)

    def length(at: int) -> int:
        return int.from_bytes(data[at : at + 4], "little")

    def shorten(doc: int, parents: tuple[int, ...]) -> bool:
        at, end, named = doc + 4, doc + length(doc), None
        while at < end - 1:
            kind, key_end = data[at], data.index(0, at + 1)
            key, at = data[at + 1 : key_end].decode(), key_end + 1
            if kind in (0x03, 0x04):
                if shorten(at, (*parents, doc)):
                    return True
                at += length(at)
            elif kind == 0x02:
                if key == "owner":
                    named = data[at + 4 : at + 3 + length(at)].decode()
                at += 4 + length(at)
            elif kind == 0x05:
                size = length(at)
                if key == "data" and named == owner:
                    del data[at + 4 + size]
                    for start in (*parents, doc, at):
                        data[start : start + 4] = (length(start) - 1).to_bytes(4, "little")
                    return True
                at += 5 + size
            else:
                at += BSON_FIXED_SIZES[kind]
        return False

    assert shorten(0, ())
    return bytes(data)


def adc_on(description: MachineDescription, cs: str) -> SpiTargetBase:
    """A new `conftest.Adc` on chip select `cs`, the class the description attaches on SPI1."""
    (entry,) = description.board["SPI1"].devices
    return cast(Callable[..., SpiTargetBase], entry.cls)(cs=cs)


def adc(machine: Machine) -> Framing:
    (device,) = machine.board.devices("SPI1")
    assert type(device).__name__ == "Adc"
    assert isinstance(device, Framing)
    return device


def samples(machine: Machine, channel: int) -> list[int]:
    row = machine.var("adc_result").samples[channel]
    return [int(row[index].value) for index in range(CONVERSIONS)]


def received(machine: Machine) -> list[int]:
    result = machine.var("rng_result")
    assert result.count.value == COUNT
    return [int(result.words[index].value) for index in range(COUNT)]


async def test_without_the_plugin_a_strict_run_names_the_rng(
    native_plugin_description: MachineDescription,
) -> None:
    machine = Machine(
        native_plugin_description.part_description(),
        image=[image.load() for image in native_plugin_description.images],
        strict_peripherals=True,
    )
    await native_plugin_description.boot_machine(machine)
    with pytest.raises(ModelError, match="RNG"):
        await machine.run(for_=ms(20), budget=BUDGET)


async def test_the_firmware_gets_the_seeded_sequence(native_plugin: Machine) -> None:
    await run_to_done(native_plugin)
    assert received(native_plugin) == list(itertools.islice(words(1), COUNT))


async def test_hal_rng_init_writes_nscr_into_the_description(
    native_plugin: Machine, caplog: pytest.LogCaptureFixture
) -> None:
    await run_to_done(native_plugin)
    assert native_plugin.periph.RNG.NSCR.value == RNG_NSCR_NIST_VALUE
    assert not [r for r in caplog.records if "no register there" in r.getMessage()]


async def test_each_later_word_waits_one_conversion(native_plugin: Machine) -> None:
    callbacks: list[VirtualTime] = []

    def record(call: Call) -> Return:
        callbacks.append(native_plugin.time)
        return call.proceed()

    native_plugin.intercept("HAL_RNG_ReadyDataCallback", record)
    await run_to_done(native_plugin)
    assert len(callbacks) == COUNT
    for earlier, later in itertools.pairwise(callbacks):
        assert CONVERSION <= later - earlier < 2 * CONVERSION


async def test_condrst_cancels_the_conversion_and_clears_sr(native_plugin: Machine) -> None:
    await run_to_done(native_plugin)
    rng = native_plugin.periph.RNG
    rng.CR.write(rng.CR.value | 1 << RNGEN_BIT)
    stop = await native_plugin.run(for_=2 * CONVERSION)
    assert stop.reason is StopReason.TIME, stop
    assert rng.SR.DRDY == 1
    assert rng.DR.value == next(itertools.islice(words(1), COUNT, None))
    rng.CR.write(rng.CR.value | 1 << CONDRST_BIT)
    assert rng.SR.value == 0
    assert rng.DR.value == 0
    rng.CR.write(rng.CR.value & ~(1 << CONDRST_BIT | 1 << RNGEN_BIT))
    rng.CR.write(rng.CR.value | 1 << RNGEN_BIT)
    rng.CR.write(rng.CR.value | 1 << CONDRST_BIT)
    stop = await native_plugin.run(for_=2 * CONVERSION)
    assert stop.reason is StopReason.TIME, stop
    assert rng.SR.value == 0


async def test_the_adc_buffer_follows_a_ramp_in_virtual_time(native_plugin: Machine) -> None:
    await run_to_adc_done(native_plugin)
    assert native_plugin.var("adc_result").conversions.value == CONVERSIONS
    step = adc(native_plugin).counts_per_ms
    rising, mirrored = samples(native_plugin, 0), samples(native_plugin, 1)
    for earlier, later in itertools.pairwise(rising):
        assert step <= later - earlier < 3 * step
    for up, down in zip(rising, mirrored, strict=True):
        assert 0 <= FULL_SCALE - down - up < step


async def test_each_conversion_is_one_chip_select_period(native_plugin: Machine) -> None:
    await run_to_adc_done(native_plugin)
    device = adc(native_plugin)
    assert device.selects == device.deselects == CONVERSIONS * CHANNELS
    assert device.bad_frames == 0


async def test_a_restore_brings_back_the_words_and_the_adc_counters(
    native_plugin: Machine,
) -> None:
    for _ in range(COUNT // 2):
        stop = await native_plugin.run(until="HAL_RNG_ReadyDataCallback", for_=ms(20))
        assert stop.reason is StopReason.ADDRESS, stop
    mid_rng = native_plugin.snapshot()
    await run_to_done(native_plugin)
    expected = list(itertools.islice(words(1), COUNT))
    assert received(native_plugin) == expected
    native_plugin.restore(mid_rng)
    await run_to_done(native_plugin)
    assert received(native_plugin) == expected
    stop = await native_plugin.run(for_=ms(5))
    assert stop.reason is StopReason.TIME, stop
    device = adc(native_plugin)
    counted = framing(device)
    assert 0 < counted[0] < CONVERSIONS * CHANNELS
    mid_adc = native_plugin.snapshot()
    await run_to_adc_done(native_plugin)
    done, rising = framing(device), samples(native_plugin, 0)
    native_plugin.restore(mid_adc)
    assert framing(device) == counted
    await run_to_adc_done(native_plugin)
    assert framing(device) == done
    assert samples(native_plugin, 0) == rising


async def test_two_snapshots_of_one_parked_state_are_equal(native_plugin: Machine) -> None:
    stop = await native_plugin.run(until="HAL_RNG_ReadyDataCallback", for_=ms(20))
    assert stop.reason is StopReason.ADDRESS, stop
    first = native_plugin.snapshot()
    assert native_plugin.snapshot().to_bytes() == first.to_bytes()
    await run_to_done(native_plugin)
    native_plugin.restore(first)
    restored = native_plugin.snapshot()
    assert native_plugin.snapshot().to_bytes() == restored.to_bytes()


async def test_a_short_rng_state_in_a_snapshot_is_refused(native_plugin: Machine) -> None:
    saved = native_plugin.snapshot().to_bytes()
    with pytest.raises(ConfigError, match="a snapshot of 12 bytes, not 11"):
        native_plugin.restore(Snapshot.from_bytes(short_model_state(saved, "RNG")))


async def test_a_reset_restarts_the_sequence_from_the_seed(
    native_plugin: Machine, native_plugin_description: MachineDescription
) -> None:
    await run_to_done(native_plugin)
    native_plugin.reset()
    rng = native_plugin.periph.RNG
    assert rng.CR.value == 0
    assert rng.DR.value == 0
    await native_plugin_description.boot_machine(native_plugin)
    await run_to_done(native_plugin)
    assert received(native_plugin) == list(itertools.islice(words(1), COUNT))


def test_clocked_by_finds_the_adc_by_its_chip_select(
    native_plugin_description: MachineDescription,
) -> None:
    on_ss = SpiTarget()
    machine = Machine(
        native_plugin_description.part_description(),
        devices={"SPI1": [adc_on(native_plugin_description, "PB0"), on_ss]},
        audio={
            "SAI1": {
                "A": AudioOptions(clocked_by="SPI1:PB0"),
                "B": AudioOptions(clocked_by="SPI1:SS"),
            }
        },
    )
    on_ss.board.set_audio_clock("SAI1", "B", 44_100)
    assert machine.periph.SAI1.audio("B").external_frame_hz == 44_100
    with pytest.raises(ConfigError):
        on_ss.board.set_audio_clock("SAI1", "A", 48_000)


def test_clocked_by_ss_does_not_name_the_adc_on_pb0(
    native_plugin_description: MachineDescription,
) -> None:
    with pytest.raises(ConfigError) as refused:
        Machine(
            native_plugin_description.part_description(),
            devices={"SPI1": [adc_on(native_plugin_description, "PB0")]},
            audio={"SAI1": {"A": AudioOptions(clocked_by="SPI1:SS")}},
        )
    assert refused.value.key == "audio.SAI1.A.clocked_by"


def test_an_adc_on_a_gpio_port_the_part_lacks_is_refused(
    native_plugin_description: MachineDescription,
) -> None:
    with pytest.raises(ConfigError) as refused:
        Machine(
            native_plugin_description.part_description(),
            devices={"SPI1": [adc_on(native_plugin_description, "PZ4")]},
        )
    assert refused.value.key == "devices.SPI1"
    assert refused.value.expected == "a chip select on a GPIO port of STM32U575QGIx"
