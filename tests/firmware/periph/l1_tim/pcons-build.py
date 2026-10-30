from pathlib import Path

from popcorn_build.stm32_firmware import STM32L152XB, STM32L152XE, add_ll_firmware

here = Path(__file__).resolve().parent

for chipset in (STM32L152XE, STM32L152XB):
    add_ll_firmware(
        "l1_tim_ll",
        [here / "l1_tim_ll.c"],
        entries=[
            "idle",
            "tim2_update",
            "tim2_pwm",
            "tim3_capture",
            "tim3_capture_div2",
            "tim2_clocks_tim3",
            "tim5_remap_tim2",
            "on_update",
            "on_compare",
            "on_started",
        ],
        chipset=chipset,
    )
