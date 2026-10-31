from pathlib import Path

from popcorn_build.stm32_firmware import STM32L152XE, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "l1_iwdg_ll",
    [here / "l1_iwdg_ll.c"],
    entries=["idle", "refresh", "updates"],
    chipset=STM32L152XE,
)
