"""The built-in RNG word source: seed, generator, queue and stuck word, through rng_ll."""

from __future__ import annotations

import asyncio
import struct
from collections.abc import Callable
from pathlib import Path

import popcorn
import pytest
from popcorn._rng import MAX_QUEUED
from popcorn.board import RngEntropy, RngOptions, rng_words
from popcorn.units import ms

from .conftest import StorageModel, run_bounded

RNG_ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "rng_ll.elf"
RNG_DR = 0x420C_0808
SEC_RNG_DR = 0x520C_0808
CR_RESET = 0x0080_0D00
CONDRST = 1 << 30
RNGEN = 1 << 2
HSI48ON = 1 << 12
AHB_RNGEN = 1 << 18
WORDS = 16
LOGGED = 3 * WORDS + 1
SEED_0_WORDS = [0xE220_A839, 0x6E78_9E6A, 0x06C4_5D18, 0xF88B_B8A8]
MASK64 = (1 << 64) - 1
GAMMA = 0x9E37_79B9_7F4A_7C15
PLAIN_DIGEST = "c1fef57e3020d0713b9ead07c1fab2394e8855dce16a87439f02662500bcd068"


def entered(
    u575: popcorn.PartDescription, entry: str, rng: RngOptions | None = None
) -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(RNG_ELF), strict_peripherals=True, rng=rng)
    m.reset()
    assert asyncio.run(m.run(until="main", budget=100_000)).reason is popcorn.StopReason.ADDRESS
    m.reg.pc = m.sym(entry)
    return m


def logged(m: popcorn.Machine) -> list[int]:
    count = m.mem.u32[m.sym("log_len")]
    base = m.sym("log_buf")
    return [m.mem.u32[base + 4 * index] for index in range(count)]


def fw_words(m: popcorn.Machine) -> list[int]:
    log = logged(m)
    assert len(log) == LOGGED
    assert all(log[3 * word] == 1 for word in range(WORDS))
    return [log[3 * word + 2] for word in range(WORDS)]


def parked(m: popcorn.Machine) -> list[int]:
    assert asyncio.run(m.run(for_=ms(50), budget=200_000)).reason is popcorn.StopReason.TIME
    return fw_words(m)


def running(m: popcorn.Machine) -> list[int]:
    async def body() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(m.ref.var("log_len").ge(LOGGED))

    assert run_bounded(body(), 30).reason is popcorn.StopReason.USER
    return fw_words(m)


def unmix(z: int) -> int:
    """The SplitMix64 input whose finalizer gives `z`."""
    z ^= z >> 31 ^ z >> 62
    z = (z * pow(0x94D0_49BB_1331_11EB, -1, 1 << 64)) & MASK64
    z ^= z >> 27 ^ z >> 54
    z = (z * pow(0xBF58_476D_1CE4_E5B9, -1, 1 << 64)) & MASK64
    z ^= z >> 30 ^ z >> 60
    return z


def test_rng_words_of_seed_0_are_fixed() -> None:
    assert rng_words(0, 4) == SEED_0_WORDS
    assert rng_words(0, 2, start=2) == SEED_0_WORDS[2:]
    assert rng_words(0, 0) == []


@pytest.mark.parametrize("seed", [0, 0x5EED])
def test_the_firmware_reads_rng_words_parked_and_running(
    u575: popcorn.PartDescription, seed: int
) -> None:
    options = RngOptions(seed) if seed else None
    expected = rng_words(seed, WORDS)
    assert parked(entered(u575, "enable_read_idle", options)) == expected
    assert running(entered(u575, "enable_read_idle", options)) == expected


def second_read(m: popcorn.Machine) -> None:
    m.mem.u32[m.sym("log_len")] = 0
    m.reg.pc = m.sym("enable_read_idle")


@pytest.mark.parametrize("read", [parked, running])
def test_drawn_after_a_16_word_read_is_where_the_next_read_starts(
    u575: popcorn.PartDescription, read: Callable[[popcorn.Machine], list[int]]
) -> None:
    m = entered(u575, "enable_read_idle")
    assert read(m) == rng_words(0, WORDS)
    assert m.periph.RNG.entropy.drawn == WORDS
    second_read(m)
    assert read(m) == rng_words(0, WORDS, start=WORDS)


def test_the_python_queue_bound_is_the_native_one() -> None:
    assert MAX_QUEUED == 1 << 16


def test_a_machine_description_seed_gives_the_words_of_machine_rng(
    u575: popcorn.PartDescription,
) -> None:
    description = popcorn.MachineDescription(
        part="STM32U575QGIx", images=[popcorn.Elf(RNG_ELF)], rng=RngOptions(0x5EED), strict=True
    )
    m = description.machine()
    m.reset()
    assert asyncio.run(m.run(until="main", budget=100_000)).reason is popcorn.StopReason.ADDRESS
    m.reg.pc = m.sym("enable_read_idle")
    assert m.periph.RNG.entropy.seed == 0x5EED
    assert parked(m) == parked(entered(u575, "enable_read_idle", RngOptions(0x5EED)))


def test_queued_words_give_a_chosen_64_byte_key(u575: popcorn.PartDescription) -> None:
    key = bytes(range(0xA0, 0xE0))
    chosen = list(struct.unpack("<16I", key))
    m = entered(u575, "enable_read_idle")
    m.periph.RNG.entropy.queue(chosen)
    assert m.periph.RNG.entropy.queued == WORDS
    assert struct.pack("<16I", *parked(m)) == key
    assert m.periph.RNG.entropy.queued == 0
    assert m.periph.RNG.entropy.drawn == 0


def test_a_generated_zero_is_skipped() -> None:
    seed = (unmix(0x1234_5678) - GAMMA) & MASK64
    assert rng_words(seed, 1) != [0]
    assert rng_words(seed, 1) == rng_words(seed, 1, start=1)


def test_the_firmware_skips_a_generated_zero(u575: popcorn.PartDescription) -> None:
    seed = (unmix(0x1234_5678) - GAMMA) & MASK64
    m = entered(u575, "enable_read_idle", RngOptions(seed))
    words = parked(m)
    assert 0 not in words
    assert words == rng_words(seed, WORDS) == rng_words(seed, WORDS, start=1)
    assert m.periph.RNG.entropy.drawn >= WORDS + 1


def test_a_dr_watch_sees_the_words_the_firmware_reads(u575: popcorn.PartDescription) -> None:
    m = entered(u575, "enable_read_idle")
    watches = [
        m.watch(at, kind=popcorn.WatchKind.READ, handler=lambda _: None)
        for at in (RNG_DR, SEC_RNG_DR)
    ]
    words = parked(m)
    for watch in watches:
        assert [hit.value for hit in watch.hits] == words
        assert all(hit.initiator is popcorn.Initiator.CPU for hit in watch.hits)


def test_a_host_read_of_dr_consumes_no_word(u575: popcorn.PartDescription) -> None:
    m = entered(u575, "enable_read_idle")
    first = m.watch(RNG_DR, kind=popcorn.WatchKind.READ)
    stop = asyncio.run(m.run(for_=ms(50), budget=200_000))
    assert stop.reason is popcorn.StopReason.WATCHPOINT
    first.remove()
    head = m.periph.RNG.DR.value
    assert head == SEED_0_WORDS[1]
    assert m.mem.u32[RNG_DR] == head
    assert m.mem.u32[SEC_RNG_DR] == head
    assert m.periph.RNG.DR.value == head
    assert parked(m) == rng_words(0, WORDS)


def test_a_snapshot_after_8_words_gives_the_same_next_8(u575: popcorn.PartDescription) -> None:
    m = entered(u575, "enable_read_idle")
    stop = asyncio.run(m.wait_for(m.ref.var("log_len").ge(24), for_=ms(50)))
    assert stop.reason is popcorn.StopReason.USER
    saved = m.snapshot()
    first = parked(m)
    other = popcorn.Machine(u575, image=popcorn.Image.elf(RNG_ELF), strict_peripherals=True)
    other.restore(popcorn.Snapshot.from_bytes(saved.to_bytes()))
    assert parked(other) == first == rng_words(0, WORDS)
    assert other.periph.RNG.entropy.drawn == m.periph.RNG.entropy.drawn


def test_refusals_change_nothing(u575: popcorn.PartDescription) -> None:
    for value, error in [(True, TypeError), (-1, ValueError), (1 << 64, ValueError)]:
        with pytest.raises(error):
            RngOptions(value)
    with pytest.raises(TypeError):
        rng_words(0, True)
    with pytest.raises(ValueError):
        rng_words(0, (1 << 20) + 1)
    entropy = entered(u575, "idle").periph.RNG.entropy
    entropy.queue([1, 2])
    for words, error in [([3, 1 << 32], ValueError), ([3, True], TypeError), ([-1], ValueError)]:
        with pytest.raises(error):
            entropy.queue(words)
        assert (entropy.queued, entropy.drawn) == (2, 0)
    with pytest.raises(ValueError):
        entropy.queue([0] * (MAX_QUEUED - 1))
    assert entropy.queued == 2
    entropy.queue([0] * (MAX_QUEUED - 2))
    assert entropy.queued == MAX_QUEUED
    for stuck, error in [(True, TypeError), (1 << 32, ValueError), (-1, ValueError)]:
        with pytest.raises(error):
            entropy.stuck = stuck
        assert entropy.stuck is None
    entropy.stuck = 0
    assert entropy.stuck == 0
    entropy.stuck = None
    assert entropy.stuck is None


def test_a_board_model_on_rng_has_no_entropy_and_refuses_rng(
    u575: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(u575, models={"RNG": StorageModel})
    entropy = m.periph.RNG.entropy
    assert isinstance(entropy, RngEntropy)
    with pytest.raises(popcorn.ModelError):
        _ = entropy.seed
    with pytest.raises(popcorn.ConfigError) as refused:
        popcorn.Machine(u575, models={"RNG": StorageModel}, rng=RngOptions(1))
    assert refused.value.key == "rng"
    with pytest.raises(TypeError):
        popcorn.Machine(u575, rng=1)  # pyright: ignore[reportArgumentType]


def test_the_digest_includes_rng_only_when_set() -> None:
    plain = popcorn.MachineDescription(part="STM32U575QGIx", images=[])
    assert plain.digest == PLAIN_DIGEST
    assert plain.replace(rng=None).digest == PLAIN_DIGEST
    seeded = {plain.replace(rng=RngOptions(seed)).digest for seed in (0, 1)}
    assert len(seeded) == 2
    assert plain.digest not in seeded
    with pytest.raises(popcorn.ConfigError):
        popcorn.MachineDescription(part="STM32U575QGIx", images=[], rng=1)  # pyright: ignore


def test_1000_queue_and_restart_cycles_keep_the_queue_empty(
    u575: popcorn.PartDescription,
) -> None:
    m = entered(u575, "idle")
    rcc = m.periph.RCC
    rcc.RCC_CR.write(rcc.RCC_CR.value | HSI48ON)
    rcc.RCC_AHB2ENR1.write(rcc.RCC_AHB2ENR1.value | AHB_RNGEN)
    asyncio.run(m.run(for_=ms(1)))
    rng = m.periph.RNG
    entropy = rng.entropy
    for cycle in range(1000):
        entropy.queue([cycle + 1])
        assert entropy.queued == 1
        rng.CR.write(CR_RESET | CONDRST)
        rng.CR.write(CR_RESET | RNGEN)
        asyncio.run(m.run(for_=ms(1)))
        assert entropy.queued == 0
        assert rng.DR.value == cycle + 1
    assert entropy.drawn == 3 * 1000


def keygen_parked(m: popcorn.Machine) -> int:
    assert asyncio.run(m.run(for_=ms(10), budget=500_000)).reason is popcorn.StopReason.TIME
    assert m.mem.u32[m.sym("keygen_done")] == 1
    return m.mem.u32[m.sym("keygen_status")]


def keygen_running(m: popcorn.Machine) -> int:
    async def body() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(m.ref.var("keygen_done").ge(1))

    assert run_bounded(body(), 30).reason is popcorn.StopReason.USER
    return m.mem.u32[m.sym("keygen_status")]


@pytest.mark.parametrize("run", [keygen_parked, keygen_running])
def test_keygen_reports_each_injected_failure(
    u575: popcorn.PartDescription, run: Callable[[popcorn.Machine], int]
) -> None:
    m = entered(u575, "keygen")
    assert run(m) == 0
    key = [m.mem.u32[m.sym("keygen_key") + 4 * word] for word in range(WORDS)]
    assert key == rng_words(0, WORDS)
    for name, status in [("seed_fault", 1), ("clock_fault", 2)]:
        m = entered(u575, "keygen")
        setattr(m.periph.RNG.entropy, name, True)
        assert run(m) == status
    m = entered(u575, "keygen")
    m.periph.RNG.entropy.stuck = 0x1234_5678
    assert run(m) == 3


@pytest.mark.parametrize("run", [keygen_parked, keygen_running])
def test_a_fault_after_n_reads_shows_after_n_good_words(
    u575: popcorn.PartDescription, run: Callable[[popcorn.Machine], int]
) -> None:
    m = entered(u575, "keygen")
    entropy = m.periph.RNG.entropy
    entropy.seed_fault_after = 8
    assert entropy.seed_fault_after == 8
    assert run(m) == 1
    assert entropy.seed_fault
    assert entropy.seed_fault_after is None
    key = [m.mem.u32[m.sym("keygen_key") + 4 * word] for word in range(WORDS)]
    assert key == [*rng_words(0, 8), *[0] * 8]
    m = entered(u575, "keygen")
    entropy = m.periph.RNG.entropy
    entropy.clock_fault_after = 6
    assert run(m) == 2
    key = [m.mem.u32[m.sym("keygen_key") + 4 * word] for word in range(WORDS)]
    assert key == [*rng_words(0, 6), *[0] * 10]
    assert entropy.clock_fault
    assert entropy.clock_fault_after is None
    assert m.periph.RNG.SR.CECS == 0
    assert m.periph.RNG.SR.CEIS == 1


def test_fault_refusals_change_nothing(u575: popcorn.PartDescription) -> None:
    entropy = entered(u575, "idle").periph.RNG.entropy
    for name in ("seed_fault", "clock_fault"):
        for value in (1, None, "yes"):
            with pytest.raises(TypeError):
                setattr(entropy, name, value)
            assert getattr(entropy, name) is False
    for name in ("seed_fault_after", "clock_fault_after"):
        for value, error in [
            (0, ValueError),
            (-1, ValueError),
            (1 << 64, ValueError),
            (True, TypeError),
        ]:
            with pytest.raises(error):
                setattr(entropy, name, value)
            assert getattr(entropy, name) is None
        setattr(entropy, name, 3)
        assert getattr(entropy, name) == 3
        setattr(entropy, name, None)
        assert getattr(entropy, name) is None


NIST_CR = 0x00F0_0D00
NIST_NSCR = 0x1_7CBB
NIST_HTCR = 0xA2B0
CONFIGLOCK = 1 << 31
DEINIT_LOGGED = 31


def deinit_parked(m: popcorn.Machine) -> list[int]:
    assert asyncio.run(m.run(for_=ms(10), budget=500_000)).reason is popcorn.StopReason.TIME
    return logged(m)


def deinit_running(m: popcorn.Machine) -> list[int]:
    async def body() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(m.ref.var("log_len").ge(DEINIT_LOGGED))

    assert run_bounded(body(), 30).reason is popcorn.StopReason.USER
    return logged(m)


@pytest.mark.parametrize("run", [deinit_parked, deinit_running])
def test_ll_rng_deinit_returns_a_locked_rng_to_reset_and_it_works_again(
    u575: popcorn.PartDescription, run: Callable[[popcorn.Machine], list[int]]
) -> None:
    m = entered(u575, "deinit")
    m.periph.RNG.entropy.seed_fault_after = 50
    log = run(m)
    assert len(log) == DEINIT_LOGGED
    assert log[2:9:2] == rng_words(0, 4)
    locked = NIST_CR | CONFIGLOCK | RNGEN
    assert log[9] == locked
    assert log[11:13] == [NIST_NSCR, NIST_HTCR]
    assert log[13:18] == [CR_RESET, 0, 0x3_FFFF, 0x72AC, 0]
    assert 0 < log[18] < 2000
    assert log[19] == log[1] > 100
    assert log[20:27:2] == rng_words(0, 4, start=4)
    assert log[27] == locked
    assert log[29:31] == [NIST_NSCR, NIST_HTCR]
    assert m.periph.RNG.entropy.seed_fault_after == 42
