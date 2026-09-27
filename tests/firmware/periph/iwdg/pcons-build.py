from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "iwdg_ll", [here / "iwdg_ll.c"], ["rcc"], ["idle", "refresh", "window", "early_wakeup"]
)
