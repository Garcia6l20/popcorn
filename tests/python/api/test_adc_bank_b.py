"""STM32L1 ADC bank B inputs, `ADC.input(n, bank="B")`: the channels on a pad of their own, RM0038
Table 41, bonded by the description's `bonded_channels_b`, refused on every other ADC."""

from __future__ import annotations

import asyncio
from pathlib import Path
from typing import Any, cast

import popcorn
import pytest
from popcorn import ConfigError, StopReason, _core
from popcorn.board import AnalogBank
from popcorn.stm32 import mcu
from popcorn.units import us

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
XE_ADC = PREBUILT / "l152xe" / "l1_adc_ll.elf"
XB_ADC = PREBUILT / "l152xb" / "l1_adc_ll.elf"
RCC_ELF = PREBUILT / "rcc_ll.elf"
ADON = 1 << 0
ADC_CFG = 1 << 2
SWSTART = 1 << 30
NATIVE_HANDLE = "_handle"
OWN_PADS = "0b 1b 2b 3b 6b 7b 8b 9b 10b 11b 12b"


@pytest.fixture(scope="module")
def l152re() -> popcorn.PartDescription:
    return mcu("STM32L152RETx")


@pytest.fixture(scope="module")
def l152rc() -> popcorn.PartDescription:
    return mcu("STM32L152RCTx")


@pytest.fixture(scope="module")
def l152rb() -> popcorn.PartDescription:
    return mcu("STM32L152RBTx")


@pytest.fixture(scope="module")
def h573() -> popcorn.PartDescription:
    return mcu("STM32H573AIIx")


def idle(description: popcorn.PartDescription, image: Path = XE_ADC) -> popcorn.Machine:
    m = popcorn.Machine(description, image=popcorn.Image.elf(image))
    m.reset()
    m.reg.pc = m.sym("idle")
    return m


def settle(m: popcorn.Machine, duration: int) -> None:
    assert asyncio.run(m.run(for_=us(duration))).reason is StopReason.TIME


def convert(m: popcorn.Machine, channel: int, control: int) -> int:
    adc = m.periph.ADC
    adc.SQR5.write(channel)
    adc.CR2.write(ADON | control | SWSTART)
    settle(m, 40)
    return adc.DR.value


def test_the_shipped_l152_parts_bond_bank_b_by_package(
    l152re: popcorn.PartDescription,
    l152rc: popcorn.PartDescription,
    l152rb: popcorn.PartDescription,
) -> None:
    def mask(description: popcorn.PartDescription) -> int | None:
        return next(p.bonded_channels_b for p in description.peripherals if p.name == "ADC")

    assert mask(l152re) == 0x0001
    assert mask(l152rc) == 0x0001
    assert mask(l152rb) is None


def test_a_bank_b_drive_reaches_an_adc_cfg_1_conversion_and_leaves_bank_a(
    l152re: popcorn.PartDescription,
) -> None:
    m = idle(l152re)
    m.periph.RCC.CR.write(0x301)
    settle(m, 200)
    m.periph.ADC.CR2.write(ADON)
    settle(m, 4)
    own = m.periph.ADC.input(0, bank="B")
    own.volts = 1.5
    m.periph.ADC.input(0).volts = 0.75
    assert convert(m, 0, ADC_CFG) == 2048
    assert convert(m, 0, 0) == 1024
    assert own.volts == pytest.approx(1.5)
    assert m.periph.ADC.input(0).volts == pytest.approx(0.75)
    assert repr(own) == "AnalogInput(ADC, 0b)"
    assert own.bank == "B"
    saved = m.snapshot()
    own.volts = 0.0
    m.restore(saved)
    assert own.volts == pytest.approx(1.5)
    m.reset()
    assert own.volts == pytest.approx(1.5)


@pytest.mark.parametrize(
    ("channel", "message", "expected"),
    [
        (
            13,
            "ADC: channel 13b shares the pad of bank A channel 13",
            "a bank B channel on a pad of its own, one of 0b, 1b, 2b, 3b, 6b, 7b, 8b, 9b, 10b, "
            "11b, 12b",
        ),
        (
            26,
            "ADC: no external channel 26b",
            "a bank B channel on a pad of its own, one of 0b, 1b, 2b, 3b, 6b, 7b, 8b, 9b, 10b, "
            "11b, 12b",
        ),
        (1, "ADC: channel 1b reaches no package pin", "a bonded bank B channel, one of 0b"),
    ],
)
def test_a_bank_b_channel_refused_on_the_l152re_names_the_rule_parked_and_running(
    l152re: popcorn.PartDescription, channel: int, message: str, expected: str
) -> None:
    m = idle(l152re)

    def refused() -> ConfigError:
        with pytest.raises(ConfigError) as caught:
            m.periph.ADC.input(channel, bank="B")
        with pytest.raises(ConfigError):
            m.ref.periph.ADC.input(channel, bank="B")
        return caught.value

    parked = refused()

    async def body() -> ConfigError:
        async with m.running():
            return refused()

    running = asyncio.run(body())
    for error in (parked, running):
        assert str(error).startswith(message)
        assert error.key == "channel"
        assert error.expected == expected
    assert m.periph.ADC.input(channel if channel != 26 else 0).volts == 0.0


@pytest.mark.parametrize(
    ("channel", "text"),
    [
        (1, "a bonded channel of STM32L152RETx, ADC channel 1b reaches no package pin, bonded: 0b"),
        (
            13,
            f"a bank B channel on a pad of its own, one of {OWN_PADS}, not 13b, "
            "which shares the pad of bank A channel 13",
        ),
        (
            26,
            f"a bank B channel on a pad of its own, one of {OWN_PADS}, not 26b",
        ),
    ],
)
def test_the_native_entries_refuse_the_same_bank_b_channels_at_the_call(
    l152re: popcorn.PartDescription, channel: int, text: str
) -> None:
    m = idle(l152re)
    handle: Any = getattr(m, NATIVE_HANDLE)
    bank = _core.AdcBank.b

    ops = (
        lambda: handle.op_adc_drive("ADC", channel, bank, 1_000_000),
        lambda: handle.op_adc_input("ADC", channel, bank),
    )
    for access in (
        lambda: handle.adc_drive("ADC", channel, bank, 1_000_000),
        lambda: handle.adc_input("ADC", channel, bank),
        *ops,
    ):
        with pytest.raises(ConfigError) as caught:
            access()
        assert text in str(caught.value)

    async def body() -> None:
        async with m.running():
            for access in ops:
                with pytest.raises(ConfigError):
                    access()

    asyncio.run(body())
    assert handle.adc_channels("ADC", bank) == 0x1FCF
    assert handle.adc_bonded_channels("ADC", bank) == 0x0001


def test_an_unknown_bank_is_refused(l152re: popcorn.PartDescription) -> None:
    m = idle(l152re)
    with pytest.raises(ConfigError) as caught:
        m.periph.ADC.input(0, bank=cast(AnalogBank, "C"))
    assert caught.value.key == "bank"
    assert caught.value.expected == '"A" or "B"'
    with pytest.raises(TypeError):
        cast(Any, m.periph.ADC).input(0, "B")
    with pytest.raises(TypeError):
        cast(Any, m.ref.periph.ADC).input(0, "B")


@pytest.mark.parametrize(
    ("part", "adc", "image"),
    [("l152rb", "ADC", XB_ADC), ("u575", "ADC1", None), ("h573", "ADC2", None)],
)
def test_an_adc_with_one_bank_refuses_bank_b(
    request: pytest.FixtureRequest, part: str, adc: str, image: Path | None
) -> None:
    description: popcorn.PartDescription = request.getfixturevalue(part)
    m = popcorn.Machine(description) if image is None else idle(description, image)
    view = m.periph[adc]
    with pytest.raises(ConfigError) as caught:
        view.input(3, bank="B")
    assert str(caught.value).startswith(f"{adc}: no bank B")
    assert caught.value.key == "bank"
    with pytest.raises(ConfigError):
        m.ref.periph[adc].input(3, bank="B")
    handle: Any = getattr(m, NATIVE_HANDLE)
    with pytest.raises(ConfigError) as native:
        handle.adc_drive(adc, 3, _core.AdcBank.b, 1_000_000)
    assert native.value.key == "bank"
    assert handle.adc_channels(adc, _core.AdcBank.b) == 0


def test_the_bank_b_ref_label_and_drive(l152re: popcorn.PartDescription) -> None:
    m = idle(l152re)
    bank: AnalogBank = "B"
    ref = m.ref.periph.ADC.input(0, bank=bank)
    assert 'periph.ADC.input(0, bank="B").volts' in repr(ref.volts)

    async def body() -> None:
        async with m.running():
            await m.write((m.ref.periph.ADC.input(0, bank="B").volts, 1.25))

    asyncio.run(body())
    assert m.periph.ADC.input(0, bank="B").volts == pytest.approx(1.25)
    assert m.periph.ADC.input(0).volts == 0.0


@pytest.mark.parametrize(
    ("part", "adc", "image", "channel"),
    [("u575", "ADC1", RCC_ELF, 0), ("l152re", "ADC", XE_ADC, 16), ("l152re", "ADC", XE_ADC, 32)],
)
def test_a_bank_a_channel_outside_the_external_mask_is_refused_at_the_call_parked_and_running(
    request: pytest.FixtureRequest, part: str, adc: str, image: Path, channel: int
) -> None:
    m = idle(request.getfixturevalue(part), image)
    handle: Any = getattr(m, NATIVE_HANDLE)
    bank = _core.AdcBank.a

    def refused() -> None:
        for access in (
            lambda: handle.op_adc_drive(adc, channel, bank, 1_000_000),
            lambda: handle.op_adc_input(adc, channel, bank),
        ):
            with pytest.raises(ConfigError, match=f"an external channel, one of .*, not {channel}"):
                access()

    refused()
    with pytest.raises(ConfigError, match=f"not {channel}"):
        handle.adc_drive(adc, channel, bank, 1_000_000)

    async def body() -> None:
        async with m.running():
            refused()

    asyncio.run(body())
