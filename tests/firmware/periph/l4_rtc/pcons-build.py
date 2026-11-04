from pathlib import Path

from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "l4_rtc_ll",
    [here / "l4_rtc_ll.c"],
    ["rtc"],
    entries=[
        "idle",
        "calendar",
        "read_calendar",
        "read_after_reset",
        "backup_write",
        "backup_read",
        "backup_reset",
    ],
    chipset=STM32L476XX,
)
