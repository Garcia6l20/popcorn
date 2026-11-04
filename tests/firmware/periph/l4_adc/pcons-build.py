from pathlib import Path

from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "l4_adc_ll", [here / "l4_adc_ll.c"], entries=["idle", "aden_at_once"], chipset=STM32L476XX
)
