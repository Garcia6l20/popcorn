from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "mdf_ll",
    [here / "mdf_ll.c"],
    [],
    [
        "idle",
        "two_mics",
        "two_mics_lf",
        "two_mics_restart",
        "independent_irq",
        "single_shot",
        "manchester",
        "adf_sad",
    ],
)
