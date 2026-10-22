"""A 64-byte key from the built-in RNG, LL: words from a seed, chosen words and injected faults.

The firmware starts HSI48, enables the RNG bus clock and the RNG, reads 16 words with DRDY polled,
and checks SECS, SEIS, CECS, CEIS, a zero word and two equal words in a row before keeping each
one. It stores the words and a status in `rng_key`, turns the RNG and HSI48 off, then stops at
`rng_done`. The project sets `MachineDescription(strict=True)`.

popcorn's RNG takes its words from a seed, 0 unless `rng=RngOptions(seed=...)` sets one, so a
test predicts the key with `rng_words`. `m.periph.RNG.entropy` queues chosen words, sticks every
word at one value and injects the seed and clock errors.
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
RNG_DR = 0x420C_0808


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
    rcc = rng.periph.RCC
    assert rng.periph.RNG.CR.RNGEN == 0
    assert rcc.RCC_CR.HSI48ON == 0
    assert rcc.RCC_CR.HSI48RDY == 0
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


async def test_hsi48_left_off_is_a_clock_error(rng: Machine) -> None:
    rng.var("rng_use_hsi48").value = 0
    status, key = await generated(rng)
    assert status == CLOCK_ERROR
    assert key == []
