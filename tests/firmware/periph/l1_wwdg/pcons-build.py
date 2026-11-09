from pathlib import Path

from popcorn_build.stm32_firmware import STM32L152XE, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "l1_wwdg_ll",
    [here / "l1_wwdg_ll.c"],
    entries=["idle", "refresh", "arm"],
    chipset=STM32L152XE,
)
