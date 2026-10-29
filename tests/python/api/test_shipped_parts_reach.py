"""Every shipped part reaches the flash, SRAM, pins and ADC channels of its package, with no
firmware.

The flash size comes from the ordering code, the SRAM ends from the reference manuals and the
refused pins from the description's own `bonded` masks, so each check has an oracle other than
the region or mask it tests. Bonding against ST's pin data is the freshness test's.
"""

from __future__ import annotations

import popcorn
import pytest
from popcorn.stm32 import shipped_parts

FLASH_START = 0x0800_0000
FLASH_KIB = {"G": 1024, "I": 2048}
FLASHSIZE_ADDRESS = {"U5": 0x0BFA_07A0, "H5": 0x08FF_F80C}
SRAM_RANGES = {
    "U5": {
        "SRAM_NS": (0x2000_0000, 0x200C_0000),
        "SRAM_SRD_NS": (0x2800_0000, 0x2800_4000),
        "BKPSRAM_NS": (0x4003_6400, 0x4003_6C00),
    },
    "H5": {
        "SRAM_NS": (0x2000_0000, 0x200A_0000),
        "BKPSRAM_NS": (0x4003_6400, 0x4003_7400),
    },
}
EXTERNAL_CHANNELS = {"U5": set(range(1, 18)), "H5": {*range(16), 18, 19}}
CODE_ALIAS = "RAM_C_NS"
ADC_CHANNELS = 32


def family(part: str) -> str:
    return "U5" if part.startswith(("STM32U575", "STM32U585")) else "H5"


def flash_kib(part: str) -> int:
    return FLASH_KIB[part[10]]


def build(part: str) -> popcorn.Machine:
    machine = popcorn.MachineDescription(part=part, images=[]).machine()
    machine.reset()
    return machine


def covered(machine: popcorn.Machine, address: int) -> bool:
    description = machine.description
    return any(r.start <= address < r.start + r.size for r in description.regions) or any(
        p.base <= address < p.base + p.size for p in description.peripherals
    )


@pytest.mark.parametrize("part", shipped_parts())
def test_flashsize_matches_the_flash_region_and_the_ordering_code(part: str) -> None:
    machine = build(part)
    region = next(r for r in machine.description.regions if r.name == "Flash_NS")
    assert machine.mem.u16[FLASHSIZE_ADDRESS[family(part)]] == flash_kib(part)
    assert region.size == flash_kib(part) * 1024


@pytest.mark.parametrize("part", shipped_parts())
def test_the_last_flash_quad_word_round_trips_and_the_next_address_refuses(part: str) -> None:
    machine = build(part)
    end = FLASH_START + flash_kib(part) * 1024
    region = next(r for r in machine.description.regions if r.name == "Flash_NS")
    assert region.start + region.size == end
    assert machine.mem.read(end - 16, 16) == b"\xff" * 16
    pattern = bytes(range(16))
    machine.mem.write(end - 16, pattern)
    assert machine.mem.read(end - 16, 16) == pattern
    assert not covered(machine, end)
    with pytest.raises(popcorn.BusAccessError):
        machine.mem.read(end, 4)


@pytest.mark.parametrize("part", shipped_parts())
def test_the_last_word_of_every_sram_round_trips(part: str) -> None:
    machine = build(part)
    ram = {
        r.name: (r.start, r.start + r.size)
        for r in machine.description.regions
        if r.name.endswith("_NS") and r.kind.name == "ram" and r.name != CODE_ALIAS
    }
    assert ram == SRAM_RANGES[family(part)]
    for name, (start, end) in ram.items():
        for address, value in ((start, 0x5A3C_C3A5), (end - 4, 0xA5C3_0F1E)):
            machine.mem.u32[address] = value
            assert machine.mem.u32[address] == value, name
        assert not covered(machine, end), name
        with pytest.raises(popcorn.BusAccessError):
            machine.mem.u32[end]


@pytest.mark.parametrize("part", shipped_parts())
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
                    assert bool(machine.mem.u32[port.base + 0x10] >> number & 1) == level
            else:
                with pytest.raises(popcorn.ConfigError) as refused:
                    view.pin(number).level = True
                assert refused.value.key == "pin"


@pytest.mark.parametrize("part", shipped_parts())
def test_every_adc1_channel_outside_bonded_channels_refuses(part: str) -> None:
    machine = build(part)
    adc = next(p for p in machine.description.peripherals if p.name == "ADC1")
    if adc.bonded_channels is None:
        pytest.skip(f"{part} lists no bonded ADC1 channels")
    external = EXTERNAL_CHANNELS[family(part)]
    for number in range(ADC_CHANNELS):
        if adc.bonded_channels >> number & 1 and number in external:
            machine.periph.ADC1.input(number).volts = 1.0
            continue
        reason = "reaches no package pin" if number in external else "no external channel"
        with pytest.raises(popcorn.ConfigError, match=reason) as refused:
            machine.periph.ADC1.input(number)
        assert refused.value.key == "channel"
