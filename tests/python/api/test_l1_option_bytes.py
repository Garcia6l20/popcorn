"""`Machine(option_bytes=)` on the STM32L1, RM0038 3.3.2 and Table 19: the flash base boots, or
the bank 2 base with `swap_bank` on a part with two banks, and the TrustZone fields are refused.
The FLASH unit is found by the program memory it controls on every family."""

from __future__ import annotations

from pathlib import Path

import popcorn
import pytest
from popcorn import ConfigError, OptionBytes
from popcorn.model import Model
from popcorn.stm32 import mcu, shipped_parts

ROOT = Path(__file__).parents[3]
XE_ELF = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "l152xe" / "l1_smoke.elf"
OBR = 0x4002_3C00 + 0x1C
NBFB2 = 1 << 23
BANK2 = 0x0804_0000


@pytest.fixture(scope="module")
def l152re() -> popcorn.PartDescription:
    return mcu("STM32L152RETx")


def booted(
    description: popcorn.PartDescription, option_bytes: OptionBytes | None
) -> popcorn.Machine:
    m = popcorn.Machine(description, image=popcorn.Image.elf(XE_ELF), option_bytes=option_bytes)
    m.reset()
    return m


def test_default_option_bytes_boot_from_the_flash_base(l152re: popcorn.PartDescription) -> None:
    m = booted(l152re, OptionBytes())
    plain = booted(l152re, None)
    assert m.reg.pc == plain.reg.pc == m.mem.u32[0x0800_0004] & ~1
    assert m.reg.sp == m.mem.u32[0x0800_0000]
    assert m.mem.u32[OBR] & NBFB2 == NBFB2


def test_swap_bank_boots_the_vector_table_of_bank_2(l152re: popcorn.PartDescription) -> None:
    m = popcorn.Machine(
        l152re, image=popcorn.Image.elf(XE_ELF), option_bytes=OptionBytes(swap_bank=True)
    )
    handler = m.mem.u32[0x0800_0004]
    m.mem.u32[BANK2] = 0x2000_4000
    m.mem.u32[BANK2 + 4] = handler
    m.reset()
    assert m.reg.pc == handler & ~1
    assert m.reg.sp == 0x2000_4000
    assert m.mem.u32[OBR] & NBFB2 == 0


@pytest.mark.parametrize(
    ("option_bytes", "key"),
    [
        (OptionBytes(tzen=True), "option_bytes.tzen"),
        (OptionBytes(secwm1=(0, 1)), "option_bytes.secwm1"),
        (OptionBytes(secwm2=None), "option_bytes.secwm2"),
        (OptionBytes(secboot=0x0800_0000), "option_bytes.secboot"),
        (OptionBytes(nsboot=0x0800_0000), "option_bytes.nsboot"),
        (OptionBytes(dualbank=True), "option_bytes.dualbank"),
    ],
)
def test_fields_without_an_l1_meaning_are_refused(
    l152re: popcorn.PartDescription, option_bytes: OptionBytes, key: str
) -> None:
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(l152re, option_bytes=option_bytes)
    assert caught.value.key == key


SWAP_BANK_ACCEPTED = {"Cat.4": "STM32L152RDTx", "Cat.5": "STM32L152RETx", "Cat.6": "STM32L151VDTxX"}
"""A part of each category with two banks, RM0038 3.2 Tables 10 to 12."""
SWAP_BANK_REFUSED = {
    "Cat.1": "STM32L152RBTx",
    "Cat.2": "STM32L152RBTxA",
    "Cat.3 DIE427": "STM32L152RCTx",
    "Cat.3 DIE436": "STM32L152RCTxA",
}
"""A part of each category with one bank, RM0038 3.2 Tables 8 and 9."""


@pytest.mark.parametrize("part", SWAP_BANK_ACCEPTED.values(), ids=SWAP_BANK_ACCEPTED.keys())
def test_swap_bank_on_a_two_bank_part_loads_nbfb2_0(part: str) -> None:
    m = popcorn.Machine(mcu(part), option_bytes=OptionBytes(swap_bank=True))
    assert m.mem.u32[OBR] & NBFB2 == 0


@pytest.mark.parametrize("part", SWAP_BANK_REFUSED.values(), ids=SWAP_BANK_REFUSED.keys())
def test_swap_bank_on_a_one_bank_part_is_refused(part: str) -> None:
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(mcu(part), option_bytes=OptionBytes(swap_bank=True))
    assert caught.value.key == "option_bytes.swap_bank"


def test_a_python_model_on_the_l1_flash_is_refused(l152re: popcorn.PartDescription) -> None:
    class Plain(Model):
        pass

    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(l152re, models={"FLASH": Plain}, option_bytes=OptionBytes())
    assert caught.value.key == "option_bytes"
    assert caught.value.expected == "the built-in model of FLASH, not a Python model"
    assert popcorn.Machine(l152re, models={"FLASH": Plain})


@pytest.mark.parametrize(
    "part", [p for p in shipped_parts() if p.startswith(("STM32U5", "STM32H5"))]
)
def test_the_flash_unit_is_the_ip_flash_unit_on_every_u5_and_h5_part(part: str) -> None:
    class Plain(Model):
        pass

    description = mcu(part)
    oracle = next(p.name for p in description.peripherals if p.ip == "flash" and p.alias_of is None)
    with pytest.raises(ConfigError) as caught:
        popcorn.Machine(description, models={oracle: Plain}, option_bytes=OptionBytes())
    assert caught.value.key == "option_bytes"
    assert caught.value.expected.startswith(f"the built-in model of {oracle},")
