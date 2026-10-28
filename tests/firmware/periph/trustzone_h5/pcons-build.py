from pathlib import Path

from popcorn_build.stm32_firmware import STM32H573, add_trustzone_cmsis_firmware

here = Path(__file__).resolve().parent

add_trustzone_cmsis_firmware(
    "tz_pair",
    [here / "tz_pair_s.c"],
    [here / "tz_pair.c"],
    here / "tz_pair_s.ld",
    here / "tz_pair.ld",
    STM32H573,
)
