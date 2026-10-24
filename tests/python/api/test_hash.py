"""The built-in HASH model against hashlib, through register writes and the hash_ll firmware."""

from __future__ import annotations

import asyncio
import hashlib
import hmac
import random
import struct
from dataclasses import dataclass
from pathlib import Path

import popcorn
import pytest
from popcorn.units import ms

from .conftest import run_bounded

HASH_ELF = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "hash_ll.elf"
ALGORITHMS = ("sha1", "md5", "sha224", "sha256")
DIGEST_SIZES = (20, 16, 28, 32)
LENGTHS = (0, 3, 64, 68, 300)
TICK_CYCLES = 4000
INIT = 1 << 2
MODE = 1 << 6
LKEY = 1 << 16
DCAL = 1 << 8
RESULTS = len(ALGORITHMS) * len(LENGTHS)


def pattern(size: int) -> bytes:
    return bytes((index * 37 + 11) & 0xFF for index in range(size))


def bit_reversed(word: int) -> int:
    return int(f"{word:032b}"[::-1], 2)


def din_word(message_word: int, datatype: int) -> int:
    match datatype:
        case 1:
            return ((message_word << 16) | (message_word >> 16)) & 0xFFFF_FFFF
        case 2:
            return int.from_bytes(message_word.to_bytes(4, "big"), "little")
        case 3:
            return bit_reversed(message_word)
        case _:
            return message_word


def entered(u575: popcorn.PartDescription, entry: str) -> popcorn.Machine:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(HASH_ELF))
    m.reset()
    assert asyncio.run(m.run(until="main", budget=1_000_000)).reason is popcorn.StopReason.ADDRESS
    m.reg.pc = m.sym(entry)
    return m


def register_hash(
    m: popcorn.Machine, algo: int, message: bytes, datatype: int, rng: random.Random
) -> bytes:
    unit = m.periph.HASH
    unit.CR.write(INIT | (algo << 17) | (datatype << 4))
    for at in range(0, len(message), 4):
        chunk = message[at : at + 4]
        valid = 8 * len(chunk)
        word = int.from_bytes(chunk.ljust(4, b"\0"), "big")
        if valid < 32:
            word |= rng.getrandbits(32) & (0xFFFF_FFFF >> valid)
        unit.DIN.write(din_word(word, datatype))
    unit.STR.write((8 * len(message)) % 32)
    unit.STR.write(DCAL)
    assert unit.SR.BUSY == 1
    assert asyncio.run(m.run(for_=ms(1))).reason is popcorn.StopReason.TIME
    assert unit.SR.DCIS == 1
    words = [unit[f"HR{index}"].read() for index in range(8)]
    assert [unit[f"HRA{index}"].read() for index in range(5)] == words[:5]
    size = DIGEST_SIZES[algo]
    assert words[size // 4 :] == [0] * (8 - size // 4)
    return struct.pack(">8I", *words)[:size]


@pytest.mark.parametrize("algo", range(len(ALGORITHMS)), ids=ALGORITHMS)
def test_random_messages_written_from_python_match_hashlib(
    u575: popcorn.PartDescription, algo: int
) -> None:
    m = entered(u575, "idle")
    rng = random.Random(0x4A5 + algo)
    for _ in range(200):
        message = rng.randbytes(rng.randrange(301))
        datatype = rng.randrange(4)
        got = register_hash(m, algo, message, datatype, rng)
        assert got == hashlib.new(ALGORITHMS[algo], message).digest(), (len(message), datatype)


@dataclass(frozen=True, slots=True)
class Results:
    digests: tuple[bytes, ...]
    cycles: tuple[int, ...]
    ticks: tuple[int, ...]
    dinis_irqs: tuple[int, ...]
    dcis_irqs: tuple[int, ...]
    busy_feeds: tuple[int, ...]


def words(m: popcorn.Machine, name: str, count: int) -> tuple[int, ...]:
    return struct.unpack(f"<{count}I", m.mem.read(m.sym(name), 4 * count))


def results(m: popcorn.Machine) -> Results:
    assert words(m, "hash_count", 1) == (RESULTS,)
    raw = words(m, "hash_digest", 8 * RESULTS)
    return Results(
        digests=tuple(
            struct.pack(">8I", *raw[8 * index : 8 * index + 8]) for index in range(RESULTS)
        ),
        cycles=words(m, "hash_cycles", RESULTS),
        ticks=words(m, "hash_ticks", RESULTS),
        dinis_irqs=words(m, "hash_dinis_irqs", RESULTS),
        dcis_irqs=words(m, "hash_dcis_irqs", RESULTS),
        busy_feeds=words(m, "hash_busy_feeds", RESULTS),
    )


def parked(u575: popcorn.PartDescription, entry: str) -> Results:
    m = entered(u575, entry)
    stop = asyncio.run(m.wait_for(m.ref.var("hash_done").ge(1), for_=ms(2000)))
    assert stop.reason is popcorn.StopReason.USER
    return results(m)


def running(u575: popcorn.PartDescription, entry: str) -> Results:
    m = entered(u575, entry)

    async def body() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(m.ref.var("hash_done").ge(1))

    assert run_bounded(body(), 30).reason is popcorn.StopReason.USER
    return results(m)


def cases() -> list[tuple[int, int]]:
    return [(algo, length) for algo in range(len(ALGORITHMS)) for length in LENGTHS]


def check_digests(found: Results) -> None:
    for index, (algo, length) in enumerate(cases()):
        size = DIGEST_SIZES[algo]
        digest = found.digests[index]
        assert digest[:size] == hashlib.new(ALGORITHMS[algo], pattern(length)).digest(), (
            ALGORITHMS[algo],
            length,
        )
        assert digest[size:] == bytes(32 - size)


def polled_waits(length: int) -> int:
    words_written = (length + 3) // 4
    return len(range(17, words_written, 16)) + 1


def irq_feeds(length: int) -> int:
    left = (length + 3) // 4
    feeds = 1
    first = True
    while left > 16:
        left -= 17 if first else 16
        first = False
        feeds += 1
    return feeds


@pytest.mark.timeout(40)
@pytest.mark.parametrize("entry", ["hash_poll", "hash_backtoback", "hash_irq"])
def test_the_firmware_hashes_alike_parked_and_running(
    u575: popcorn.PartDescription, entry: str
) -> None:
    first = parked(u575, entry)
    check_digests(first)
    assert running(u575, entry) == first
    lengths = [length for _, length in cases()]
    if entry == "hash_poll":
        assert first.ticks == tuple(polled_waits(length) for length in lengths)
        assert all(cycles % TICK_CYCLES < 100 for cycles in first.cycles)
    if entry == "hash_irq":
        assert first.dinis_irqs == tuple(irq_feeds(length) for length in lengths)
        assert first.dcis_irqs == (1,) * RESULTS
        assert first.busy_feeds == (0,) * RESULTS


def test_words_written_while_busy_keep_one_pending_event(u575: popcorn.PartDescription) -> None:
    m = entered(u575, "idle")
    unit = m.periph.HASH
    before = m.pending_events
    unit.CR.write(INIT | (3 << 17) | (2 << 4))
    message = pattern(16_384)
    for at in range(0, len(message), 4):
        unit.DIN.write(int.from_bytes(message[at : at + 4], "little"))
    unit.STR.write(0)
    unit.STR.write(DCAL)
    assert unit.SR.BUSY == 1
    assert m.pending_events == before + 1
    assert asyncio.run(m.run(for_=ms(100))).reason is popcorn.StopReason.TIME
    assert m.pending_events == before
    words = [unit[f"HR{index}"].read() for index in range(8)]
    assert struct.pack(">8I", *words) == hashlib.sha256(message).digest()


def test_init_dcal_loop_keeps_one_pending_event(u575: popcorn.PartDescription) -> None:
    m = entered(u575, "idle")
    unit = m.periph.HASH
    before = m.pending_events
    for _ in range(1000):
        unit.CR.write(INIT | (3 << 17))
        unit.STR.write(DCAL)
        assert m.pending_events == before + 1
    assert unit.SR.BUSY == 1
    assert asyncio.run(m.run(for_=ms(1))).reason is popcorn.StopReason.TIME
    assert m.pending_events == before
    assert unit.SR.BUSY == 0
    words = [unit[f"HR{index}"].read() for index in range(8)]
    assert struct.pack(">8I", *words) == hashlib.sha256(b"").digest()


DMA_CASES = [
    (algo, length, split)
    for algo in range(len(ALGORITHMS))
    for split, length in (
        (False, 68),
        (False, 300),
        (True, 68),
        (True, 300),
        (False, 67),
        (False, 66),
    )
]
DMAE = 1 << 3


def dma_results(m: popcorn.Machine) -> tuple[tuple[bytes, ...], tuple[int, ...]]:
    assert words(m, "hash_dma_count", 1) == (len(DMA_CASES),)
    raw = words(m, "hash_dma_digest", 8 * len(DMA_CASES))
    digests = tuple(
        struct.pack(">8I", *raw[8 * index : 8 * index + 8]) for index in range(len(DMA_CASES))
    )
    return digests, words(m, "hash_dma_control", len(DMA_CASES))


@pytest.mark.timeout(40)
def test_dma_input_hashes_alike_parked_and_running(u575: popcorn.PartDescription) -> None:
    m = entered(u575, "hash_dma")
    stop = asyncio.run(m.wait_for(m.ref.var("hash_done").ge(1), for_=ms(2000)))
    assert stop.reason is popcorn.StopReason.USER
    digests, controls = dma_results(m)
    for (algo, length, split), digest, control in zip(DMA_CASES, digests, controls, strict=True):
        size = DIGEST_SIZES[algo]
        expected = hashlib.new(ALGORITHMS[algo], pattern(length)).digest()
        assert digest == expected + bytes(32 - size), (ALGORITHMS[algo], length, split)
        assert control & DMAE == 0

    other = entered(u575, "hash_dma")

    async def body() -> popcorn.Stop:
        async with other.running():
            return await other.wait_for(other.ref.var("hash_done").ge(1))

    assert run_bounded(body(), 30).reason is popcorn.StopReason.USER
    assert dma_results(other) == (digests, controls)


def key_pattern(size: int) -> bytes:
    return bytes((index * 53 + 7) & 0xFF for index in range(size))


def feed_words(
    m: popcorn.Machine, data: bytes, words: range, datatype: int, rng: random.Random
) -> None:
    unit = m.periph.HASH
    for index in words:
        chunk = data[4 * index : 4 * index + 4]
        valid = 8 * len(chunk)
        word = int.from_bytes(chunk.ljust(4, b"\0"), "big")
        if valid < 32:
            word |= rng.getrandbits(32) & (0xFFFF_FFFF >> valid)
        unit.DIN.write(din_word(word, datatype))


def close_phase(m: popcorn.Machine, data: bytes, last: bool) -> None:
    unit = m.periph.HASH
    unit.STR.write((8 * len(data)) % 32)
    unit.STR.write(DCAL)
    assert unit.SR.BUSY == 1
    assert asyncio.run(m.run(for_=ms(1))).reason is popcorn.StopReason.TIME
    assert bool(unit.SR.DCIS) is last
    assert unit.SR.DINIS == 1


def register_phase(
    m: popcorn.Machine, data: bytes, datatype: int, rng: random.Random, last: bool
) -> None:
    feed_words(m, data, range((len(data) + 3) // 4), datatype, rng)
    close_phase(m, data, last)


def register_hmac(
    m: popcorn.Machine, algo: int, key: bytes, message: bytes, datatype: int, rng: random.Random
) -> bytes:
    unit = m.periph.HASH
    long_key = LKEY if len(key) > 64 else 0
    unit.CR.write(INIT | MODE | long_key | (algo << 17) | (datatype << 4))
    register_phase(m, key, datatype, rng, last=False)
    register_phase(m, message, datatype, rng, last=False)
    register_phase(m, key, datatype, rng, last=True)
    words = [unit[f"HR{index}"].read() for index in range(8)]
    assert [unit[f"HRA{index}"].read() for index in range(5)] == words[:5]
    size = DIGEST_SIZES[algo]
    assert words[size // 4 :] == [0] * (8 - size // 4)
    return struct.pack(">8I", *words)[:size]


@pytest.mark.parametrize("algo", range(len(ALGORITHMS)), ids=ALGORITHMS)
def test_random_hmacs_written_from_python_match_hmac(
    u575: popcorn.PartDescription, algo: int
) -> None:
    m = entered(u575, "idle")
    rng = random.Random(0x4AC + algo)
    for _ in range(60):
        key = rng.randbytes(rng.randrange(151))
        message = rng.randbytes(rng.randrange(301))
        datatype = rng.randrange(4)
        got = register_hmac(m, algo, key, message, datatype, rng)
        expected = hmac.new(key, message, ALGORITHMS[algo]).digest()
        assert got == expected, (len(key), len(message), datatype)


HMAC_CASES = [
    (algo, key, length)
    for algo in range(len(ALGORITHMS))
    for key, length in ((20, 0), (21, 68), (64, 300), (131, 50))
]
HMAC_DMA_CASES = [
    (algo, key, length) for algo in range(len(ALGORITHMS)) for key, length in ((20, 68), (131, 300))
]


@dataclass(frozen=True, slots=True)
class HmacResults:
    digests: tuple[bytes, ...]
    dinis_irqs: tuple[int, ...]


def hmac_results(m: popcorn.Machine, entry: str) -> HmacResults:
    if entry == "hmac_dma":
        count, digest_name = len(HMAC_DMA_CASES), "hmac_dma_digest"
        assert words(m, "hmac_dma_count", 1) == (count,)
        irqs: tuple[int, ...] = ()
    else:
        count, digest_name = len(HMAC_CASES), "hmac_digest"
        assert words(m, "hmac_count", 1) == (count,)
        irqs = words(m, "hmac_dinis_irqs", count)
    raw = words(m, digest_name, 8 * count)
    digests = tuple(struct.pack(">8I", *raw[8 * index : 8 * index + 8]) for index in range(count))
    return HmacResults(digests=digests, dinis_irqs=irqs)


def hmac_run(u575: popcorn.PartDescription, entry: str, *, parked: bool) -> HmacResults:
    m = entered(u575, entry)
    if parked:
        stop = asyncio.run(m.wait_for(m.ref.var("hash_done").ge(1), for_=ms(2000)))
        assert stop.reason is popcorn.StopReason.USER
        return hmac_results(m, entry)

    async def body() -> popcorn.Stop:
        async with m.running():
            return await m.wait_for(m.ref.var("hash_done").ge(1))

    assert run_bounded(body(), 30).reason is popcorn.StopReason.USER
    return hmac_results(m, entry)


@pytest.mark.timeout(40)
@pytest.mark.parametrize("entry", ["hmac_poll", "hmac_irq", "hmac_dma"])
def test_the_firmware_computes_hmacs_alike_parked_and_running(
    u575: popcorn.PartDescription, entry: str
) -> None:
    first = hmac_run(u575, entry, parked=True)
    cases = HMAC_DMA_CASES if entry == "hmac_dma" else HMAC_CASES
    for (algo, key, length), digest in zip(cases, first.digests, strict=True):
        size = DIGEST_SIZES[algo]
        expected = hmac.new(key_pattern(key), pattern(length), ALGORITHMS[algo]).digest()
        assert digest == expected + bytes(32 - size), (ALGORITHMS[algo], key, length)
    if entry == "hmac_irq":
        assert first.dinis_irqs == tuple(
            2 * irq_feeds(key) + irq_feeds(length) for _, key, length in cases
        )
    if entry == "hmac_poll":
        assert first.dinis_irqs == (0,) * len(cases)
    assert hmac_run(u575, entry, parked=False) == first


def test_hmac_phases_closed_back_to_back_keep_one_pending_event(
    u575: popcorn.PartDescription,
) -> None:
    m = entered(u575, "idle")
    unit = m.periph.HASH
    before = m.pending_events
    for _ in range(1000):
        unit.CR.write(INIT | MODE | (3 << 17))
        for _ in range(3):
            unit.STR.write(DCAL)
            assert m.pending_events == before + 1
    assert unit.SR.BUSY == 1
    assert asyncio.run(m.run(for_=ms(1))).reason is popcorn.StopReason.TIME
    assert m.pending_events == before
    assert unit.SR.DCIS == 1
    words = [unit[f"HR{index}"].read() for index in range(8)]
    assert struct.pack(">8I", *words) == hmac.new(b"", b"", "sha256").digest()


CR_SAVED = (1 << 3) | (3 << 4) | MODE | (1 << 13) | LKEY | (3 << 17)
HASH_CSRS = 38
HMAC_CSRS = 54


def save_context(m: popcorn.Machine, csrs: int) -> list[int]:
    unit = m.periph.HASH
    assert unit.SR.BUSY == 0
    assert unit.SR.DINIS == 1
    head = [unit.IMR.read() & 3, unit.STR.read() & 0x1F, unit.CR.read() & CR_SAVED]
    return head + [unit[f"CSR{index}"].read() for index in range(csrs)]


def restore_context(m: popcorn.Machine, saved: list[int]) -> None:
    unit = m.periph.HASH
    unit.IMR.write(saved[0])
    unit.STR.write(saved[1])
    unit.CR.write(saved[2])
    unit.CR.write(saved[2] | INIT)
    for index, word in enumerate(saved[3:]):
        unit[f"CSR{index}"].write(word)


def interrupt(m: popcorn.Machine, rng: random.Random) -> None:
    algo = rng.randrange(len(ALGORITHMS))
    message = rng.randbytes(rng.randrange(200))
    datatype = rng.randrange(4)
    if rng.randrange(2) == 0:
        got = register_hash(m, algo, message, datatype, rng)
        assert got == hashlib.new(ALGORITHMS[algo], message).digest()
        return
    key = rng.randbytes(rng.randrange(100))
    got = register_hmac(m, algo, key, message, datatype, rng)
    assert got == hmac.new(key, message, ALGORITHMS[algo]).digest()


def swapped(
    m: popcorn.Machine,
    algo: int,
    steps: list[bytes],
    keyed: bool,
    datatype: int,
    rng: random.Random,
) -> tuple[bytes, int, int]:
    unit = m.periph.HASH
    control = (algo << 17) | (datatype << 4)
    if keyed:
        control |= MODE | (LKEY if len(steps[0]) > 64 else 0)
    unit.CR.write(INIT | control)
    phase = rng.randrange(len(steps))
    full = len(steps[phase]) // 4
    at = rng.choice([0, *range(17, full + 1, 16)])
    for step, data in enumerate(steps):
        words = (len(data) + 3) // 4
        split = at if step == phase else words
        feed_words(m, data, range(split), datatype, rng)
        if step == phase:
            assert asyncio.run(m.run(for_=ms(1))).reason is popcorn.StopReason.TIME
            saved = save_context(m, HMAC_CSRS if keyed else HASH_CSRS)
            interrupt(m, rng)
            restore_context(m, saved)
            feed_words(m, data, range(split, words), datatype, rng)
        close_phase(m, data, last=step == len(steps) - 1)
    words = [unit[f"HR{index}"].read() for index in range(8)]
    assert [unit[f"HRA{index}"].read() for index in range(5)] == words[:5]
    return struct.pack(">8I", *words)[: DIGEST_SIZES[algo]], phase, at


@pytest.mark.parametrize("algo", range(len(ALGORITHMS)), ids=ALGORITHMS)
def test_contexts_swapped_from_python_resume_to_hashlib_and_hmac(
    u575: popcorn.PartDescription, algo: int
) -> None:
    m = entered(u575, "idle")
    rng = random.Random(0x9D0 + algo)
    seen: set[tuple[bool, int, bool]] = set()
    for _ in range(120):
        keyed = rng.randrange(2) == 1
        message = rng.randbytes(rng.randrange(301))
        datatype = rng.randrange(4)
        if keyed:
            key = rng.randbytes(rng.randrange(151))
            got, phase, at = swapped(m, algo, [key, message, key], True, datatype, rng)
            expected = hmac.new(key, message, ALGORITHMS[algo]).digest()
        else:
            got, phase, at = swapped(m, algo, [message], False, datatype, rng)
            expected = hashlib.new(ALGORITHMS[algo], message).digest()
        assert got == expected, (keyed, phase, at, len(message), datatype)
        seen.add((keyed, phase, at != 0))
    assert seen == {(False, 0, False), (False, 0, True)} | {
        (True, phase, inside) for phase in range(3) for inside in (False, True)
    }


SWAP_RESULTS = 32


def swap_expected() -> list[bytes]:
    out: list[bytes] = []
    for algo, name in enumerate(ALGORITHMS):
        other = ALGORITHMS[(algo + 1) % len(ALGORITHMS)]
        out.append(hashlib.new(other, pattern(68)).digest())
        out.append(hashlib.new(name, pattern(300)).digest())
        for _ in range(3):
            out.append(hmac.new(key_pattern(21), pattern(68), other).digest())
            out.append(hmac.new(key_pattern(131), pattern(300), name).digest())
    return [digest + bytes(32 - len(digest)) for digest in out]


def swap_results(m: popcorn.Machine) -> tuple[bytes, ...]:
    assert words(m, "swap_count", 1) == (SWAP_RESULTS,)
    raw = words(m, "swap_digest", 8 * SWAP_RESULTS)
    return tuple(
        struct.pack(">8I", *raw[8 * index : 8 * index + 8]) for index in range(SWAP_RESULTS)
    )


@pytest.mark.timeout(40)
def test_the_firmware_swaps_contexts_alike_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    m = entered(u575, "hash_swap")
    stop = asyncio.run(m.wait_for(m.ref.var("hash_done").ge(1), for_=ms(2000)))
    assert stop.reason is popcorn.StopReason.USER
    first = swap_results(m)
    assert list(first) == swap_expected()

    other = entered(u575, "hash_swap")

    async def body() -> popcorn.Stop:
        async with other.running():
            return await other.wait_for(other.ref.var("hash_done").ge(1))

    assert run_bounded(body(), 30).reason is popcorn.StopReason.USER
    assert swap_results(other) == first


DMA_SWAP_CASES = 2 * len(ALGORITHMS)
DMA_SWAP_RESULTS = 2 * DMA_SWAP_CASES


def dma_swap_expected() -> list[bytes]:
    out: list[bytes] = []
    for algo, name in enumerate(ALGORITHMS):
        other = ALGORITHMS[(algo + 1) % len(ALGORITHMS)]
        out.append(hashlib.new(other, pattern(68)).digest())
        out.append(hashlib.new(name, pattern(300)).digest())
        out.append(hmac.new(key_pattern(21), pattern(68), other).digest())
        out.append(hmac.new(key_pattern(131), pattern(300), name).digest())
    return [digest + bytes(32 - len(digest)) for digest in out]


def dma_swap_results(
    m: popcorn.Machine,
) -> tuple[tuple[bytes, ...], tuple[int, ...], tuple[int, ...]]:
    assert words(m, "dma_swap_count", 1) == (DMA_SWAP_RESULTS,)
    raw = words(m, "dma_swap_digest", 8 * DMA_SWAP_RESULTS)
    digests = tuple(
        struct.pack(">8I", *raw[8 * index : 8 * index + 8]) for index in range(DMA_SWAP_RESULTS)
    )
    return (
        digests,
        words(m, "dma_swap_flags", DMA_SWAP_CASES),
        words(m, "dma_swap_words", DMA_SWAP_CASES),
    )


@pytest.mark.timeout(40)
def test_the_firmware_suspends_dma_input_alike_parked_and_running(
    u575: popcorn.PartDescription,
) -> None:
    m = entered(u575, "hash_dma_swap")
    stop = asyncio.run(m.wait_for(m.ref.var("hash_done").ge(1), for_=ms(2000)))
    assert stop.reason is popcorn.StopReason.USER
    first = dma_swap_results(m)
    digests, flags, consumed = first
    assert list(digests) == dma_swap_expected()
    assert flags == (0xF,) * DMA_SWAP_CASES
    assert consumed == (33,) * DMA_SWAP_CASES

    other = entered(u575, "hash_dma_swap")

    async def body() -> popcorn.Stop:
        async with other.running():
            return await other.wait_for(other.ref.var("hash_done").ge(1))

    assert run_bounded(body(), 30).reason is popcorn.StopReason.USER
    assert dma_swap_results(other) == first
