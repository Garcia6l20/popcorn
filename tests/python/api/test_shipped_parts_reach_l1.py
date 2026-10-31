"""Every shipped STM32L1 part reaches the end of its flash, SRAM and data EEPROM and the pins of
its package, with no firmware.

The flash size comes from the ordering code: 6, 8, B, C, D and E give 32, 64, 128, 256, 384 and
512 KiB. On Cat.6 the 384 KiB are two banks of 192 KiB with a 64 KiB hole between them, RM0038
Table 12. A 512 KiB flash ends at 0x08080000, where the data EEPROM starts. The SRAM and data
EEPROM sizes and the DBGMCU DEV_ID come from the datasheets and RM0038 by the order code, in
`SRAM_KIB`, `EEPROM_KIB` and `dev_id`, not from the db the build reads. The refused pins come
from the description's own `bonded` masks.
"""

from __future__ import annotations

import popcorn
import pytest
from popcorn.stm32 import shipped_parts

L1_PARTS = [part for part in shipped_parts() if part.startswith("STM32L1")]
FLASH_KIB = {"6": 32, "8": 64, "B": 128, "C": 256, "D": 384, "E": 512}
FLASH_START = 0x0800_0000
FLASH_REGIONS = ("Flash", "Flash_Bank2")
SRAM = "SRAM"
DATA_EEPROM = "DATA_EEPROM"
BANK_HOLE = 64 * 1024
IDR = 0x10
DBGMCU_IDCODE = 0xE004_2000
DEV_ID_MASK = 0xFFF

SRAM_KIB: dict[tuple[str, str, str], int] = {
    ("00", "6", ""): 4,
    ("00", "8", ""): 8,
    ("00", "B", ""): 10,
    ("00", "6", "A"): 4,
    ("00", "8", "A"): 8,
    ("00", "B", "A"): 16,
    ("00", "C", ""): 16,
    ("5x", "6", ""): 10,
    ("5x", "8", ""): 10,
    ("5x", "B", ""): 16,
    ("5x", "6", "A"): 16,
    ("5x", "8", "A"): 32,
    ("5x", "B", "A"): 32,
    ("5x", "C", ""): 32,
    ("5x", "C", "A"): 32,
    ("5x", "D", ""): 48,
    ("5x", "D", "X"): 80,
    ("5x", "E", ""): 80,
    ("62", "C", ""): 32,
    ("62", "C", "A"): 32,
    ("62", "D", ""): 48,
    ("62", "D", "X"): 80,
    ("62", "E", ""): 80,
}
"""RAM in Kbytes by line, flash letter and order code suffix, line `5x` for the STM32L151 and
STM32L152. The device features tables: L100 DS9496 Rev 8 Table 1, DS10193 Rev 6 Table 2 and DS9821
Table 1; L15x DS6876 Table 2, DS9515 Table 2, DS8890 Table 2, DS10262 Rev 8 Table 2, DS8576 Table 2,
DS10764 Table 1 and DS10002 Table 2; L162 DS8928 Table 2, DS10287 Table 2, DS8669 Table 1, DS10765
Table 1 and DS10168 Table 2."""

EEPROM_KIB: dict[tuple[str, str], int] = {
    ("00", "6"): 2,
    ("00", "8"): 2,
    ("00", "B"): 2,
    ("00", "C"): 4,
    ("5x", "6"): 4,
    ("5x", "8"): 4,
    ("5x", "B"): 4,
    ("5x", "C"): 8,
    ("5x", "D"): 12,
    ("5x", "E"): 16,
    ("62", "C"): 8,
    ("62", "D"): 12,
    ("62", "E"): 16,
}
"""Data EEPROM in Kbytes by line and flash letter, from the tables of SRAM_KIB. The Cat.6 D-X
parts hold 16 Kbytes, DS10764 Table 1 and DS10765 Table 1, see eeprom_kib."""


def build(part: str) -> popcorn.Machine:
    machine = popcorn.MachineDescription(part=part, images=[]).machine()
    machine.reset()
    return machine


def covered(machine: popcorn.Machine, address: int) -> bool:
    description = machine.description
    return any(r.start <= address < r.start + r.size for r in description.regions) or any(
        p.base <= address < p.base + p.size for p in description.peripherals
    )


def region(machine: popcorn.Machine, name: str) -> tuple[int, int]:
    found = next(r for r in machine.description.regions if r.name == name)
    return found.start, found.start + found.size


def holder(machine: popcorn.Machine, address: int) -> str | None:
    return next(
        (r.name for r in machine.description.regions if r.start <= address < r.start + r.size),
        None,
    )


def round_trips_to_its_end(machine: popcorn.Machine, name: str, then: str | None = None) -> None:
    """The first and last words of region @p name round-trip. The next address is unmapped, or
    with @p then the start of that region."""
    start, end = region(machine, name)
    for address, value in ((start, 0x5A3C_C3A5), (end - 4, 0xA5C3_0F1E)):
        machine.mem.u32[address] = value
        assert machine.mem.u32[address] == value, name
    if then is not None and holder(machine, end) == then:
        assert region(machine, then)[0] == end
        return
    assert not covered(machine, end), name
    with pytest.raises(popcorn.BusAccessError):
        machine.mem.u32[end]


def test_the_shipped_set_holds_the_132_l1_names() -> None:
    assert len(L1_PARTS) == 132


@pytest.mark.parametrize("part", L1_PARTS)
def test_the_flash_matches_the_ordering_code_and_ends_where_it_says(part: str) -> None:
    machine = build(part)
    banks = [
        (r.start, r.start + r.size) for r in machine.description.regions if r.name in FLASH_REGIONS
    ]
    kib = FLASH_KIB[part[10]]
    assert sum(end - start for start, end in banks) == kib * 1024
    assert banks[0][0] == FLASH_START
    if len(banks) == 2:
        assert banks[1][0] - banks[0][1] == BANK_HOLE
    else:
        assert banks[0][1] == FLASH_START + kib * 1024
    for name in (r.name for r in machine.description.regions if r.name in FLASH_REGIONS):
        round_trips_to_its_end(machine, name, then=DATA_EEPROM if kib == 512 else None)


def order_code(part: str) -> tuple[str, str, str, str, str]:
    """Line group, pin count letter, flash letter, package letter and suffix of @p part,
    `STM32L151RCYx` gives `5x`, `R`, `C`, `Y` and an empty suffix."""
    line = part[7:9]
    return "5x" if line in ("51", "52") else line, part[9], part[10], part[11], part[13:]


def sram_kib(part: str) -> int:
    line, _, flash, _, suffix = order_code(part)
    return SRAM_KIB[line, flash, suffix]


def eeprom_kib(part: str) -> int:
    line, _, flash, _, suffix = order_code(part)
    return 16 if suffix == "X" else EEPROM_KIB[line, flash]


def dev_id(part: str) -> int:
    """DEV_ID of RM0038 Rev 18 30.6.1: 0x416 Cat.1, 0x429 Cat.2, the 6, 8 and B parts with the A
    suffix, 0x427 Cat.3, 0x436 Cat.4 and, note 1, the Cat.3 parts with the A suffix, in WLCSP64 or
    with more than 100 pins, 0x437 Cat.5 and Cat.6."""
    _, pins, flash, package, suffix = order_code(part)
    if flash in "68B":
        return 0x429 if suffix == "A" else 0x416
    if flash == "C":
        late = suffix == "A" or (pins, package) == ("R", "Y") or pins in "QZ"
        return 0x436 if late else 0x427
    if flash == "D" and suffix != "X":
        return 0x436
    return 0x437


def test_the_oracle_tables_name_every_shipped_l1_code() -> None:
    assert {order_code(part)[0] for part in L1_PARTS} == {"00", "5x", "62"}
    for part in L1_PARTS:
        assert sram_kib(part) and eeprom_kib(part) and dev_id(part), part


@pytest.mark.parametrize("part", L1_PARTS)
def test_the_dbgmcu_dev_id_is_the_category_of_the_order_code(part: str) -> None:
    machine = build(part)
    assert machine.mem.u32[DBGMCU_IDCODE] & DEV_ID_MASK == dev_id(part)


def test_the_wlcsp64_l151rc_has_the_instances_of_the_l151rc_a() -> None:
    names = {
        part: sorted(p.name for p in build(part).description.peripherals)
        for part in ("STM32L151RCYx", "STM32L151RCTxA", "STM32L151RCTx")
    }
    assert names["STM32L151RCYx"] == names["STM32L151RCTxA"]
    assert {"GPIOF", "GPIOG"} <= set(names["STM32L151RCYx"])
    assert names["STM32L151RCYx"] != names["STM32L151RCTx"]


@pytest.mark.parametrize("part", L1_PARTS)
def test_the_last_word_of_the_sram_and_of_the_data_eeprom_round_trips(part: str) -> None:
    machine = build(part)
    ram, eeprom = sram_kib(part) * 1024, eeprom_kib(part) * 1024
    assert region(machine, SRAM)[1] - region(machine, SRAM)[0] == ram
    assert region(machine, DATA_EEPROM)[1] - region(machine, DATA_EEPROM)[0] == eeprom
    round_trips_to_its_end(machine, SRAM)
    round_trips_to_its_end(machine, DATA_EEPROM)


@pytest.mark.parametrize("part", L1_PARTS)
def test_every_bonded_pin_drives_and_every_other_pin_refuses(part: str) -> None:
    machine = build(part)
    ports = [
        p
        for p in machine.description.peripherals
        if p.name.startswith("GPIO") and p.alias_of is None
    ]
    assert ports
    for port in ports:
        bonded = port.bonded or 0
        machine.mem.u32[port.base] = 0
        view = getattr(machine.periph, port.name)
        for number in range(16):
            if bonded >> number & 1:
                pin = view.pin(number)
                for level in (True, False, True):
                    pin.level = level
                    assert bool(machine.mem.u32[port.base + IDR] >> number & 1) == level
            else:
                with pytest.raises(popcorn.ConfigError) as refused:
                    view.pin(number).level = True
                assert refused.value.key == "pin"
