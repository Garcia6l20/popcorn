"""`Machine(option_bytes=)` on the STM32L4, BFB2 and DUALBANK of RM0351 3.7.8: `swap_bank` sets
BFB2 and, by a popcorn rule, boots the bank 2 base at its own address, `dualbank=False` clears
DUALBANK on a flash of 512 KB or less, and every other field is refused."""

from __future__ import annotations

from pathlib import Path
from typing import Any, cast

import popcorn
import pytest
from popcorn import ConfigError, OptionBytes
from popcorn.stm32 import mcu

ROOT = Path(__file__).parents[3]
SMOKE_ELF = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "l476" / "l4_smoke.elf"
OPTR = 0x4002_2000 + 0x20
OPTION_AREA = 0x1FFF_7800
PRODUCTION = 0xFFEF_F8AA
BFB2 = 1 << 20
DUALBANK = 1 << 21
BANK2 = 0x0802_0000


@pytest.fixture(scope="module")
def l476rc() -> popcorn.PartDescription:
    return mcu("STM32L476RCTx")


def test_default_option_bytes_boot_from_the_flash_base(l476rc: popcorn.PartDescription) -> None:
    m = popcorn.Machine(l476rc, image=popcorn.Image.elf(SMOKE_ELF), option_bytes=OptionBytes())
    m.reset()
    assert m.reg.pc == m.mem.u32[0x0800_0004] & ~1
    assert m.reg.sp == m.mem.u32[0x0800_0000]
    assert m.mem.u32[OPTR] == PRODUCTION


def test_swap_bank_sets_bfb2_and_boots_bank_2_at_its_address(
    l476rc: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(
        l476rc, image=popcorn.Image.elf(SMOKE_ELF), option_bytes=OptionBytes(swap_bank=True)
    )
    handler = m.mem.u32[0x0800_0004]
    m.mem.u32[BANK2] = 0x2000_4000
    m.mem.u32[BANK2 + 4] = handler
    m.reset()
    assert m.reg.pc == handler & ~1
    assert m.reg.sp == 0x2000_4000
    assert m.mem.u32[OPTR] == PRODUCTION | BFB2
    assert m.mem.u32[OPTION_AREA] == PRODUCTION | BFB2
    assert m.mem.u32[OPTION_AREA + 4] == ~(PRODUCTION | BFB2) & 0xFFFF_FFFF


def test_dualbank_false_clears_dualbank(l476rc: popcorn.PartDescription) -> None:
    m = popcorn.Machine(
        l476rc, image=popcorn.Image.elf(SMOKE_ELF), option_bytes=OptionBytes(dualbank=False)
    )
    m.reset()
    assert m.mem.u32[OPTR] == PRODUCTION & ~DUALBANK


@pytest.mark.parametrize(
    ("option_bytes", "key"),
    [
        (OptionBytes(tzen=True), "option_bytes.tzen"),
        (OptionBytes(secwm1=(0, 1)), "option_bytes.secwm1"),
        (OptionBytes(secwm2=None), "option_bytes.secwm2"),
        (OptionBytes(hdp1=(0, 0)), "option_bytes.hdp1"),
        (OptionBytes(secboot=0x0800_0000), "option_bytes.secboot"),
        (OptionBytes(nsboot=0x0800_0000), "option_bytes.nsboot"),
        (OptionBytes(swap_bank=True, dualbank=False), "option_bytes.swap_bank"),
    ],
)
def test_fields_without_an_l4_meaning_are_refused(
    l476rc: popcorn.PartDescription, option_bytes: OptionBytes, key: str
) -> None:
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(l476rc, option_bytes=option_bytes)
    assert caught.value.key == key


def test_option_bytes_take_no_field_they_do_not_name() -> None:
    with pytest.raises(TypeError, match="rdp"):
        cast(Any, OptionBytes)(rdp=0xBB)
