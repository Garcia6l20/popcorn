"""The built-in HASH model against hashlib, through register writes and the hash_ll firmware."""

from __future__ import annotations

import asyncio
import hashlib
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
    stop = asyncio.run(m.wait_for(m.ref.var("hash_done").ge(1)))
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
