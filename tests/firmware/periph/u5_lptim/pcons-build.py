from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "lptim_ll",
    [here / "lptim_ll.c"],
    entries=[
        "idle",
        "lptim1_continuous",
        "lptim1_forever",
        "lptim4_one_shot",
        "on_compare",
        "on_reload",
    ],
)
