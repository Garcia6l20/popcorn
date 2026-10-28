"""TrustZone calls: a call runs in the core's current security state."""

from __future__ import annotations

import asyncio
import struct

import popcorn
import pytest
from popcorn import OptionBytes, ValueKind

from .conftest import TzPart

NONSECURE = 0x2008_0000
SAU_CTRL = 0xE000_EDD0
SAU_RNR = 0xE000_EDD8
SAU_RBAR = 0xE000_EDDC
SAU_RLAR = 0xE000_EDE0


def secure_core(tz: TzPart) -> popcorn.Machine:
    """A TZEN=1 machine reset to Secure state, with one Non-secure SAU region at NONSECURE."""
    m = popcorn.Machine(
        tz.description, image=popcorn.Image.elf(tz.rcc_elf), option_bytes=OptionBytes(tzen=True)
    )
    m.reset()
    m.mem.u32[SAU_RNR] = 0
    m.mem.u32[SAU_RBAR] = NONSECURE
    m.mem.u32[SAU_RLAR] = (NONSECURE + 0x3E0) | 1
    m.mem.u32[SAU_CTRL] = 1
    return m


def test_a_call_to_a_non_secure_function_from_secure_state_is_refused_before_it_runs(
    tz: TzPart,
) -> None:
    """The refusal comes before anything is posted: the core stays where it was and runs on."""
    m = secure_core(tz)
    pc = m.pc
    with pytest.raises(popcorn.ConfigError) as caught:
        asyncio.run(m.call(NONSECURE | 1, returns=ValueKind.I32))
    assert caught.value.key == "entry"
    assert "Non-secure" in str(caught.value)
    assert "0x20080001" in str(caught.value)
    assert m.pc == pc
    assert asyncio.run(m.run(budget=10)).reason == popcorn.StopReason.BUDGET


ARENA = 0x6F00_0000
ARENA_SIZE = 1 << 20
MPCBB1_SECCFGR0 = 0x5003_2D00
NONSECURE_CODE = 0x2000_2000
SECURE_CODE = 0x2000_4000
SECURE_STUB = SECURE_CODE + 0x100


def test_one_arena_returns_a_secure_and_a_non_secure_call(
    tz: TzPart,
) -> None:
    """A Secure call returns to its sentinel in Non-secure memory through the fetch fault there,
    a Non-secure call needs an arena in Non-secure memory, both on the default arena."""
    m = popcorn.Machine(
        tz.description, image=popcorn.Image.elf(tz.rcc_elf), option_bytes=OptionBytes(tzen=True)
    )
    m.reset()
    m.mem.u32[MPCBB1_SECCFGR0] &= ~(1 << ((NONSECURE_CODE - 0x2000_0000) // 0x200))
    for number, (base, limit) in enumerate(
        ((NONSECURE_CODE, NONSECURE_CODE + 0x1E0), (ARENA, ARENA + ARENA_SIZE - 0x20))
    ):
        m.mem.u32[SAU_RNR] = number
        m.mem.u32[SAU_RBAR] = base
        m.mem.u32[SAU_RLAR] = limit | 1
    m.mem.u32[SAU_CTRL] = 1
    m.mem.write(NONSECURE_CODE, struct.pack("<3H", 0x202A, 0x4770, 0xBE01))
    m.mem.write(SECURE_CODE, struct.pack("<2H", 0x202B, 0x4770))
    m.mem.write(SECURE_STUB, struct.pack("<3H", 0xF382, 0x8888, 0x470C))

    secure = asyncio.run(m.call(SECURE_CODE | 1, returns=ValueKind.I32))
    assert secure.stop.reason == popcorn.StopReason.RETURNED
    assert secure.value == 43

    m.reg.r1 = NONSECURE_CODE + 4
    m.reg.r2 = NONSECURE_CODE + 0x1F0
    m.reg.pc = SECURE_STUB
    assert asyncio.run(m.run(budget=10)).reason == popcorn.StopReason.USER
    nonsecure = asyncio.run(m.call(NONSECURE_CODE | 1, returns=ValueKind.I32))
    assert nonsecure.stop.reason == popcorn.StopReason.RETURNED
    assert nonsecure.value == 42
