from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "rtc_ll",
    [here / "rtc_ll.c"],
    ["rcc", "rtc"],
    ["idle", "calendar", "alarm", "backup", "clock_only"],
)
