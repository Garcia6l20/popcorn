"""The STM32L476 DAC1 plays a triangle on PA4 and on chip, and ADC2 reads it back, HAL.

The firmware switches the core to HSI16 in voltage range 1. `HAL_DAC_ConfigChannel` sets DAC1
channel 1 to the TIM6 TRGO trigger with the output buffer on and the on-chip connection, MODE1
001, which drives both PA4 and the ADC2 channel 17 path, RM0351 Table 130 and 18.4.11.
`HAL_DACEx_TriangleWaveGenerate` adds a triangle of amplitude 7 to the 0x400 in DAC_DHR12R1, so
each TIM6 update at 1 kHz loads 0x400 plus 0, 1, ... 7 and back down into DAC_DOR1. ADC2 converts
channel 17, `ADC_CHANNEL_DAC1CH1_ADC2`, after each update into `samples`, and the firmware calls
`sampled` after 16 updates.

The model drives PA4 and the on-chip channel at DAC_DOR1 x VDDA / 4096, rounded up to the next
microvolt, so ADC2 reads DAC_DOR1 back code for code at any VDDA. `m.periph.GPIOA.pin(4).volts`
reads the pad. A board drive of PA4 yields to the DAC output with one warning. The project sets
`MachineDescription(strict=True)`.

`HAL_TIM_Base_Init` sets TIM6_EGR.UG with CR2.MMS still 000, which sends UG as the TRGO, RM0351
33.4.2, so the DAC loads 0x400 at init and the first update 0x401. The firmware writes CR2.MMS
directly, since `HAL_TIMEx_MasterConfigSynchronization` reads TIM6_SMCR, an offset no TIM6
register covers, which fails a strict run.
"""

import logging

import pytest
from popcorn import Machine, StopReason
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=5_000_000)

BASE_CODE = 0x400
TRIANGLE = [1, 2, 3, 4, 5, 6, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2]
MODE_PIN_AND_CHIP = 0b001
TIM6_TRGO = 0b000


async def sample(machine: Machine) -> list[int]:
    stop = await machine.run(until="sampled", for_=ms(50))
    assert stop.reason is StopReason.ADDRESS, stop
    codes = machine.var("samples")
    out: list[int] = []
    for index in range(len(TRIANGLE)):
        code = codes[index].value
        assert isinstance(code, int)
        out.append(code)
    return out


async def test_adc2_reads_the_triangle_back_code_for_code(dac: Machine) -> None:
    assert await sample(dac) == [BASE_CODE + step for step in TRIANGLE]


async def test_the_3v3_vdda_reads_the_same_codes(dac_3v3: Machine) -> None:
    assert await sample(dac_3v3) == [BASE_CODE + step for step in TRIANGLE]


async def test_hal_sets_the_trigger_the_wave_and_the_pin_and_chip_mode(dac: Machine) -> None:
    assert dac.periph.RCC.APB1ENR1.DAC1EN == 1
    assert dac.periph.DAC1.MCR.MODE1 == MODE_PIN_AND_CHIP
    assert dac.periph.DAC1.CR.EN1 == 1
    assert dac.periph.DAC1.CR.TEN1 == 1
    assert dac.periph.DAC1.CR.TSEL1 == TIM6_TRGO
    assert dac.periph.DAC1.CR.WAVE1 == 0b10
    assert dac.periph.DAC1.CR.MAMP1 == 0b0010
    assert dac.periph.TIM6.CR2.MMS == 0b010
    assert dac.periph.GPIOA.MODER.MODER4 == 0b11


async def test_the_init_ug_loads_the_first_triangle_step_through_mms_000(dac: Machine) -> None:
    assert dac.periph.DAC1.DOR1.value == BASE_CODE


async def test_pa4_carries_dor1_scaled_to_vdda(dac: Machine) -> None:
    await sample(dac)
    dor = dac.periph.DAC1.DOR1.value
    assert BASE_CODE <= dor <= BASE_CODE + 7
    assert dac.periph.GPIOA.pin(4).volts == pytest.approx(dor * 3.0 / 4096, abs=1e-6)


async def test_a_board_drive_of_pa4_yields_to_the_dac_with_one_warning(
    dac: Machine, caplog: pytest.LogCaptureFixture
) -> None:
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        dac.periph.GPIOA.pin(4).volts = 2.5
        assert await sample(dac) == [BASE_CODE + step for step in TRIANGLE]
        assert dac.periph.GPIOA.pin(4).volts < 1.0
    warned = [r for r in caplog.records if getattr(r, "popcorn_peripheral", None) == "GPIOA"]
    assert len(warned) == 1
