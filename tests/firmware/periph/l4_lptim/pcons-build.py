from pathlib import Path

from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "l4_lptim_ll",
    [here / "l4_lptim_ll.c"],
    entries=[
        "idle",
        "lptim1_continuous",
        "lptim1_one_shot",
        "on_compare",
        "on_reload",
        "on_started",
    ],
    chipset=STM32L476XX,
)
