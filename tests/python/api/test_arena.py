"""The call arena: allocation, typed arrays, reset with snapshots."""

from __future__ import annotations

import ctypes

import popcorn
import pytest

from .conftest import Machines


def test_4_kib_of_floats_round_trip(machines: Machines) -> None:
    m = machines()
    samples = [i * 0.25 for i in range(1024)]
    block = m.arena.alloc_array(ctypes.c_float, 1024)
    block.write(samples)
    assert block.values() == samples
    assert block.address >= m.arena.base
    assert int(block) == block.address


def test_restore_resets_the_arena_offset(machines: Machines) -> None:
    m = machines()
    m.arena.alloc(16)
    saved = m.snapshot()
    used = m.arena.used
    m.arena.alloc(1024)
    m.restore(saved)
    assert m.arena.used == used
    loaded = popcorn.Snapshot.from_bytes(saved.to_bytes())
    m.restore(loaded)
    assert m.arena.used == 0


def test_blocks_are_zeroed_aligned_and_bounded(machines: Machines) -> None:
    m = machines()
    text = m.arena.cstring("hi")
    assert m.mem.cstring(text.address) == "hi"
    block = m.arena.alloc(8, align=64)
    assert block.address % 64 == 0
    assert block.read() == bytes(8)
    with pytest.raises(popcorn.ConfigError):
        m.arena.alloc(m.arena.size)


def test_a_machine_without_an_arena_says_so(m33: popcorn.PartDescription) -> None:
    m = popcorn.Machine(m33, arena_base=None)
    with pytest.raises(popcorn.ConfigError, match="no arena"):
        _ = m.arena
