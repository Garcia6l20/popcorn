"""`Pin.volts`, `Pin.release_volts()` and `m.ref.periph.GPIOx.pin(n).volts`: the analog level of
a GPIO pin, which the ADC channels on its pad convert."""

import asyncio
import logging
import struct
from fractions import Fraction
from typing import cast

import popcorn
import pytest
from popcorn.board import SupplyOptions
from popcorn.units import us

PADS = (
    ("STM32U575QGIx", "ADC1", 5, "GPIOA", 0),
    ("STM32H573AIIx", "ADC1", 3, "GPIOA", 6),
    ("STM32L152RETx", "ADC", 0, "GPIOA", 0),
    ("STM32L476RCTx", "ADC1", 5, "GPIOA", 0),
)


def _machine(part: str) -> popcorn.Machine:
    return popcorn.MachineDescription(part=part, images=[]).machine()


@pytest.mark.parametrize(("part", "adc", "channel", "port", "pin"), PADS)
def test_pin_volts_reads_a_set_level_and_release_volts_clears_it(
    part: str, adc: str, channel: int, port: str, pin: int
) -> None:
    m = _machine(part)
    view = getattr(m.periph, port).pin(pin)
    assert view.volts == 0.0
    view.volts = 1.25
    assert view.volts == pytest.approx(1.25)
    view.level = True
    assert view.volts == pytest.approx(1.25)
    view.release()
    view.release_volts()
    assert view.volts == 0.0


@pytest.mark.parametrize(("part", "adc", "channel", "port", "pin"), PADS)
def test_an_adc_input_and_its_pad_pin_share_one_analog_level(
    part: str, adc: str, channel: int, port: str, pin: int
) -> None:
    m = _machine(part)
    analog = getattr(m.periph, adc).input(channel)
    view = getattr(m.periph, port).pin(pin)
    analog.volts = 0.5
    assert view.volts == pytest.approx(0.5)
    view.volts = 2.0
    assert analog.volts == pytest.approx(2.0)
    view.release_volts()
    assert analog.volts == 0.0


def test_the_l476rc_adc1_and_adc2_in9_read_the_one_level_of_pa4() -> None:
    m = _machine("STM32L476RCTx")
    m.periph.ADC1.input(9).volts = 1.5
    assert m.periph.ADC2.input(9).volts == pytest.approx(1.5)
    assert m.periph.GPIOA.pin(4).volts == pytest.approx(1.5)


@pytest.mark.parametrize(("part", "adc", "channel", "port", "pin"), PADS)
def test_the_volts_ref_reads_and_writes_the_pin_level(
    part: str, adc: str, channel: int, port: str, pin: int
) -> None:
    m = _machine(part)
    ref = getattr(m.ref.periph, port).pin(pin).volts
    asyncio.run(m.write((ref, 0.75)))
    assert getattr(m.periph, port).pin(pin).volts == pytest.approx(0.75)
    (read,) = asyncio.run(m.read(ref))
    assert read == pytest.approx(0.75)
    assert getattr(m.periph, adc).input(channel).volts == pytest.approx(0.75)
    with pytest.raises(TypeError, match="volts takes a float"):
        m.write((ref, True))


@pytest.mark.parametrize("through", ["setter", "ref"])
@pytest.mark.parametrize("volts", [-0.1, 4294.968, float("nan"), float("inf")])
def test_a_voltage_outside_0_to_4294_v_is_refused_by_the_setter_and_the_ref(
    volts: float, through: str
) -> None:
    m = _machine("STM32L476RCTx")
    view = m.periph.GPIOA.pin(0)
    with pytest.raises(ValueError, match="is not a voltage"):
        if through == "setter":
            view.volts = volts
        else:
            m.write((m.ref.periph.GPIOA.pin(0).volts, volts))
    assert view.volts == 0.0


@pytest.mark.parametrize("on", ["pin", "adc"])
@pytest.mark.parametrize(
    ("value", "volts"), [(1, 1.0), (0, 0.0), (Fraction(3, 4), 0.75), (2.5, 2.5)]
)
def test_an_int_or_any_real_number_sets_pin_or_adc_input_volts(
    value: float, volts: float, on: str
) -> None:
    m = _machine("STM32L476RCTx")
    view = m.periph.GPIOA.pin(0)
    analog = m.periph.ADC1.input(5)
    view.volts = 0.5
    if on == "pin":
        view.volts = value
    else:
        analog.volts = value
    assert view.volts == pytest.approx(volts)
    assert analog.volts == pytest.approx(volts)


@pytest.mark.parametrize("on", ["pin", "adc"])
@pytest.mark.parametrize("value", [True, False, "1.0", None])
def test_a_bool_or_a_non_number_set_on_pin_or_adc_input_volts_raises_type_error(
    value: object, on: str
) -> None:
    m = _machine("STM32L476RCTx")
    view = m.periph.GPIOA.pin(0)
    analog = m.periph.ADC1.input(5)
    view.volts = 0.5
    with pytest.raises(TypeError, match="volts takes a float"):
        if on == "pin":
            view.volts = cast(float, value)
        else:
            analog.volts = cast(float, value)
    assert view.volts == pytest.approx(0.5)
    assert analog.volts == pytest.approx(0.5)


def test_every_analog_access_of_an_unbonded_pin_is_refused_naming_the_part() -> None:
    m = _machine("STM32L476RCTx")
    pf3 = m.periph.GPIOF.pin(3)
    refused = "a bonded pin of STM32L476RCTx, PF3 reaches no package pin, bonded: none"
    for access in (
        lambda: pf3.volts,
        lambda: setattr(pf3, "volts", 1.0),
        pf3.release_volts,
        lambda: m.read(m.ref.periph.GPIOF.pin(3).volts),
        lambda: m.write((m.ref.periph.GPIOF.pin(3).volts, 1.0)),
    ):
        with pytest.raises(popcorn.ConfigError, match=refused):
            access()


def _pad_warnings(caplog: pytest.LogCaptureFixture) -> list[logging.LogRecord]:
    return [r for r in caplog.records if getattr(r, "popcorn_peripheral", None) == "GPIOA"]


def test_the_dac_output_wins_over_a_board_drive_of_pa4_and_release_volts_clears_the_board_slot(
    caplog: pytest.LogCaptureFixture,
) -> None:
    released = _machine("STM32L476RCTx")
    pa4 = released.periph.GPIOA.pin(4)
    pa4.volts = 1.0
    pa4.release_volts()
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        released.periph.DAC1.CR.EN1 = 1
        assert pa4.volts == 0.0
        released.periph.DAC1.CR.EN1 = 0
        assert pa4.volts == 0.0
    assert _pad_warnings(caplog) == []

    zero = _machine("STM32L476RCTx")
    pa4 = zero.periph.GPIOA.pin(4)
    pa4.volts = 0.0
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        zero.periph.DAC1.CR.EN1 = 1
        pa4.volts = 1.0
        assert pa4.volts == 0.0
        zero.periph.DAC1.CR.EN1 = 0
        assert pa4.volts == pytest.approx(1.0)
    assert len(_pad_warnings(caplog)) == 1


SRAM_WFI = 0x2000_0000
WFI_THEN_BRANCH_BACK = struct.pack("<2H", 0xBF30, 0xE7FD)
THUMB = 0x0100_0000


@pytest.mark.parametrize("vdda_mv", [3000, 3300])
def test_the_dac_drives_pa4_at_the_supply_vdda(vdda_mv: int) -> None:
    m = popcorn.MachineDescription(
        part="STM32L476RCTx", images=[], supply=SupplyOptions(vdda_mv=vdda_mv)
    ).machine()
    m.reset()
    m.mem.write(SRAM_WFI, WFI_THEN_BRANCH_BACK)
    m.reg.pc = SRAM_WFI
    m.reg.xpsr = THUMB
    m.periph.RCC.APB1ENR1.DAC1EN = 1
    m.periph.DAC1.DHR12R1.DACC1DHR = 0x800
    m.periph.DAC1.CR.EN1 = 1
    asyncio.run(m.run(for_=us(10)))
    assert m.periph.GPIOA.pin(4).volts == pytest.approx(vdda_mv / 2000)
