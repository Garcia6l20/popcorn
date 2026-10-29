"""Board-side pins outside a port's `bonded` mask, on shipped parts and on parts built from pin
data."""

from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path

import popcorn
import pytest
from popcorn.board import SpiTarget
from popcorn.stm32.description._shipped import build_text
from popcorn.units import ms

RCC_LL = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "rcc_ll.elf"
PIN_DATA_VARIABLE = "POPCORN_STM32_PIN_DATA"
NATIVE_HANDLE = "_handle"
"""The Machine attribute holding the native handle, whose calls skip the Python view checks."""


def _pin_data_text(name: str) -> str:
    value = os.environ.get(PIN_DATA_VARIABLE)
    if not value:
        pytest.skip(f"set {PIN_DATA_VARIABLE} to a STM32_open_pin_data checkout")
    return build_text(name, pin_data=Path(value))


@pytest.fixture(scope="module")
def zgtx_text() -> str:
    """STM32U575ZGTx, LQFP144, which bonds no pin of port I, as description JSON."""
    return _pin_data_text("STM32U575ZGTx")


@pytest.fixture(scope="module")
def zgtx(zgtx_text: str) -> popcorn.PartDescription:
    """STM32U575ZGTx built from pin data."""
    return popcorn.parse_description(zgtx_text, "STM32U575ZGTx")


@pytest.fixture(scope="module")
def aiix() -> popcorn.PartDescription:
    """STM32U575AIIx, UFBGA169, which bonds port I."""
    return popcorn.parse_description(_pin_data_text("STM32U575AIIx"), "STM32U575AIIx")


def _idle(description: popcorn.PartDescription) -> popcorn.Machine:
    machine = popcorn.Machine(description, image=popcorn.Image.elf(RCC_LL))
    machine.reset()
    machine.reg.pc = machine.sym("idle")
    return machine


def _bonded(description: popcorn.PartDescription) -> dict[str, int | None]:
    return {
        p.name: p.bonded
        for p in description.peripherals
        if p.name.startswith("GPIO") and p.alias_of is None
    }


def test_the_shipped_u575qgix_bonds_the_ports_of_its_package(
    u575: popcorn.PartDescription,
) -> None:
    assert {name: bin(mask or 0).count("1") for name, mask in _bonded(u575).items()} == {
        "GPIOA": 16,
        "GPIOB": 16,
        "GPIOC": 16,
        "GPIOD": 16,
        "GPIOE": 16,
        "GPIOF": 11,
        "GPIOG": 16,
        "GPIOH": 3,
        "GPIOI": 0,
    }
    assert _bonded(u575)["GPIOH"] == 0b1011


def test_u575qgix_drives_ph3_and_refuses_ph4_naming_the_part(
    u575: popcorn.PartDescription,
) -> None:
    machine = _idle(u575)
    gpioh = machine.periph.GPIOH
    gpioh.MODER.MODE3 = 0
    gpioh.pin(3).level = True
    assert gpioh.pin(3).level
    with pytest.raises(popcorn.ConfigError) as caught:
        gpioh.pin(4).level = True
    assert caught.value.key == "pin"
    assert caught.value.expected == (
        "a bonded pin of STM32U575QGIx, PH4 reaches no package pin, bonded: 0 1 3"
    )


def test_every_pin_access_of_an_unbonded_pin_is_refused_at_the_call_parked_and_running(
    zgtx: popcorn.PartDescription,
) -> None:
    assert _bonded(zgtx)["GPIOI"] == 0
    machine = _idle(zgtx)
    gpioi = machine.periph.GPIOI
    pi0 = gpioi.pin(0)
    at = machine.time + ms(1)
    refused = "a bonded pin of STM32U575ZGTx, PI0 reaches no package pin, bonded: none"
    for access in (
        lambda: pi0.level,
        lambda: setattr(pi0, "level", True),
        pi0.release,
        lambda: pi0.mode,
        lambda: pi0.alternate,
        lambda: pi0.drive_at(True, at=at),
        lambda: machine.read(machine.ref.periph.GPIOI.pin(0).level),
    ):
        with pytest.raises(popcorn.ConfigError, match=refused):
            access()

    async def running() -> None:
        async with machine.running():
            with pytest.raises(popcorn.ConfigError, match=refused):
                pi0.drive_edges([(at, True)])
            with pytest.raises(popcorn.ConfigError, match=refused):
                machine.write((machine.ref.periph.GPIOI.pin(0).level, True))

    asyncio.run(running())
    gpioi.ODR.write(0x81)
    assert gpioi.ODR.read() == 0x81


def test_u575aiix_drives_pi0(aiix: popcorn.PartDescription) -> None:
    assert _bonded(aiix)["GPIOI"] == 0xFF
    machine = _idle(aiix)
    gpioi = machine.periph.GPIOI
    gpioi.MODER.MODE0 = 0
    gpioi.pin(0).level = True
    assert gpioi.pin(0).level


def test_an_spi_chip_select_on_an_unbonded_pin_is_refused_at_machine_construction(
    zgtx: popcorn.PartDescription,
) -> None:
    with pytest.raises(
        popcorn.ConfigError,
        match="a bonded pin of STM32U575ZGTx, PI0 reaches no package pin, bonded: none",
    ):
        popcorn.Machine(zgtx, devices={"SPI1": [SpiTarget(cs="PI0")]})
    popcorn.Machine(zgtx, devices={"SPI1": [SpiTarget(cs="PA4"), SpiTarget()]})


def test_a_port_without_bonded_keeps_every_pin_of_the_port(zgtx_text: str) -> None:
    data = json.loads(zgtx_text)
    for peripheral in data["peripherals"]:
        if peripheral["name"] == "GPIOI":
            del peripheral["bonded"]
    description = popcorn.parse_description(json.dumps(data), "no bonded")
    machine = _idle(description)
    gpioi = machine.periph.GPIOI
    gpioi.MODER.MODE7 = 0
    gpioi.pin(7).level = True
    assert gpioi.pin(7).level
    with pytest.raises(popcorn.ConfigError, match="a pin of the port, 0 to 7"):
        gpioi.pin(8).level = True
    popcorn.Machine(description, devices={"SPI1": [SpiTarget(cs="PI0")]})


class SecureChipSelect(SpiTarget):
    """An SPI device whose chip select names the port through its Secure alias."""

    def __init__(self, port: str, pin: int) -> None:
        super().__init__()
        self._cs_port = port
        self._cs_pin = pin


def test_an_spi_chip_select_through_a_secure_alias_is_refused_as_the_port(
    zgtx: popcorn.PartDescription,
) -> None:
    assert any(p.name == "SEC_GPIOI" for p in zgtx.peripherals)
    with pytest.raises(
        popcorn.ConfigError,
        match="a bonded pin of STM32U575ZGTx, PI0 reaches no package pin, bonded: none",
    ):
        popcorn.Machine(zgtx, devices={"SPI1": [SecureChipSelect("SEC_GPIOI", 0)]})
    popcorn.Machine(zgtx, devices={"SPI1": [SecureChipSelect("SEC_GPIOA", 4)]})


@pytest.fixture(scope="module")
def cgtx() -> popcorn.PartDescription:
    """STM32U575CGTx, LQFP48, which bonds only PC13 to PC15 of port C."""
    return popcorn.parse_description(_pin_data_text("STM32U575CGTx"), "STM32U575CGTx")


def _channels(description: popcorn.PartDescription, adc: str) -> int | None:
    return next(p.bonded_channels for p in description.peripherals if p.name == adc)


def test_the_u575_adc1_channels_follow_the_pads_of_the_package(
    u575: popcorn.PartDescription, cgtx: popcorn.PartDescription
) -> None:
    assert _channels(u575, "ADC1") == 0x3FFFE
    assert _channels(cgtx, "ADC1") == 0x39FE0


def test_an_adc_channel_on_an_unbonded_pad_is_refused_at_every_entry(
    cgtx: popcorn.PartDescription,
) -> None:
    machine = _idle(cgtx)
    adc1 = machine.periph.ADC1
    with pytest.raises(popcorn.ConfigError) as caught:
        adc1.input(1)
    assert caught.value.expected == "a bonded channel, one of 5 6 7 8 9 10 11 12 15 16 17"
    with pytest.raises(popcorn.ConfigError):
        machine.ref.periph.ADC1.input(1)
    handle = getattr(machine, NATIVE_HANDLE)
    refused = (
        "a bonded channel of STM32U575CGTx, ADC1 channel 1 reaches no package pin, bonded: "
        "5 6 7 8 9 10 11 12 15 16 17"
    )
    for access in (
        lambda: handle.adc_drive("ADC1", 1, 1_000_000),
        lambda: handle.adc_input("ADC1", 1),
        lambda: handle.op_adc_drive("ADC1", 1, 1_000_000),
        lambda: handle.op_adc_input("ADC1", 1),
    ):
        with pytest.raises(popcorn.ConfigError, match=refused):
            access()
    adc1.input(5).volts = 1.0
    assert adc1.input(5).volts == pytest.approx(1.0)
