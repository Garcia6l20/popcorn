"""A 64-byte key from the built-in RNG, LL: words from a seed, chosen words and injected faults.

The firmware selects MSI as CLK48, the RNG kernel clock, through RCC_CCIPR.CLK48SEL, RM0351
6.4.28, enables the RNG bus clock and the RNG, reads 16 words with DRDY polled, and checks SECS,
SEIS, CECS, CEIS, a zero word and two equal words in a row before keeping each one. It stores
the words and a status in `rng_key`, turns the RNG off, then stops at `rng_done`. The project
sets `MachineDescription(strict=True)`.

popcorn's RNG takes its words from a seed, 0 unless `rng=RngOptions(seed=...)` sets one, so a
test predicts the key with `rng_words`. `m.periph.RNG.entropy` queues chosen words, sticks every
word at one value and injects the seed and clock errors. With RNG_CR.CED set, RM0351 27.7.1, the
clock error detection is off and an injected clock error leaves CECS at 0.
"""

import struct
from collections.abc import Awaitable, Callable

import pytest
from popcorn import Machine, StopReason, WatchKind
from popcorn.board import rng_words
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=1_000_000)

type Run = Callable[[Machine], Awaitable[tuple[int, list[int]]]]

WORDS = 16
OK, SEED_ERROR, CLOCK_ERROR, REPEATED = 0, 1, 2, 3
RNG_DR = 0x5006_0808
CLK48SEL_MSI = 0b11


async def generated(machine: Machine) -> tuple[int, list[int]]:
    stop = await machine.run(until="rng_done")
    assert stop.reason is StopReason.ADDRESS, stop
    return read_key(machine)


async def generated_running(machine: Machine) -> tuple[int, list[int]]:
    async with machine.running():
        stop = await machine.wait_for(machine.ref.var("rng_key").done.ge(1))
    assert stop.reason is StopReason.USER, stop
    return read_key(machine)


def read_key(machine: Machine) -> tuple[int, list[int]]:
    result = machine.var("rng_key")
    count = int(result.words.value)
    return int(result.status.value), [int(result.key[index].value) for index in range(count)]


def key_bytes(machine: Machine) -> bytes:
    return machine.mem.read(machine.var("rng_key").key.address, 4 * WORDS)


@pytest.mark.parametrize("run", [generated, generated_running])
async def test_the_key_is_the_seed_0_words(rng: Machine, run: Run) -> None:
    status, key = await run(rng)
    assert status == OK
    assert key == rng_words(0, WORDS)
    expected = b"".join(word.to_bytes(4, "little") for word in rng_words(0, WORDS))
    assert key_bytes(rng) == expected
    assert rng.periph.RNG.CR.RNGEN == 0
    assert rng.periph.RNG.CR.CED == 0
    assert rng.periph.RCC.CCIPR.CLK48SEL == CLK48SEL_MSI
    drawn = rng.periph.RNG.entropy.drawn
    await rng.run(for_=ms(1))
    assert rng.periph.RNG.entropy.drawn == drawn


async def test_a_seeded_config_gives_the_words_of_its_seed(rng_seeded: Machine) -> None:
    assert rng_seeded.periph.RNG.entropy.seed == 0x5EED
    status, key = await generated(rng_seeded)
    assert status == OK
    assert key == rng_words(0x5EED, WORDS)


async def test_queued_words_become_the_key(rng: Machine) -> None:
    chosen = bytes(range(0x40, 0x80))
    rng.periph.RNG.entropy.queue(struct.unpack("<16I", chosen))
    status, _ = await generated(rng)
    assert status == OK
    assert key_bytes(rng) == chosen


@pytest.mark.parametrize("run", [generated, generated_running])
async def test_a_seed_error_after_8_words_stops_the_key(rng: Machine, run: Run) -> None:
    rng.periph.RNG.entropy.seed_fault_after = 8
    status, key = await run(rng)
    assert status == SEED_ERROR
    assert key == rng_words(0, 8)


async def test_a_seed_error_from_the_start_leaves_no_word(rng: Machine) -> None:
    rng.periph.RNG.entropy.seed_fault = True
    status, key = await generated(rng)
    assert status == SEED_ERROR
    assert key == []


async def test_a_clock_error_is_reported(rng: Machine) -> None:
    rng.periph.RNG.entropy.clock_fault = True
    status, key = await generated(rng)
    assert status == CLOCK_ERROR
    assert key == []


async def test_ced_set_hides_the_clock_error_and_the_key_completes(rng: Machine) -> None:
    rng.var("rng_detect_clock_errors").value = 0
    rng.periph.RNG.entropy.clock_fault = True
    status, key = await generated(rng)
    assert status == OK
    assert key == rng_words(0, WORDS)
    assert rng.periph.RNG.CR.CED == 1
    assert rng.periph.RNG.SR.CECS == 0
    assert rng.periph.RNG.SR.CEIS == 0


@pytest.mark.parametrize("run", [generated, generated_running])
async def test_a_stuck_word_is_reported_as_repeated(rng: Machine, run: Run) -> None:
    rng.periph.RNG.entropy.stuck = 0x1234_5678
    status, key = await run(rng)
    assert status == REPEATED
    assert key == [0x1234_5678]


async def test_a_dr_watch_sees_every_word_of_the_key(rng: Machine) -> None:
    watch = rng.watch(RNG_DR, kind=WatchKind.READ, handler=lambda _: None)
    status, key = await generated(rng)
    assert status == OK
    assert [hit.value for hit in watch.hits] == key


async def test_no_clk48_source_is_a_clock_error(rng: Machine) -> None:
    rng.var("rng_use_msi").value = 0
    status, key = await generated(rng)
    assert status == CLOCK_ERROR
    assert key == []
    assert rng.periph.RCC.CCIPR.CLK48SEL == 0
