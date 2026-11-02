from pathlib import Path

from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "l4_tim_ll",
    [here / "l4_tim_ll.c"],
    entries=[
        "idle",
        "tim2_update",
        "tim2_pwm",
        "tim3_capture",
        "tim6_trgo",
        "tim7_update",
        "tim1_pwm",
        "on_update",
        "on_compare",
        "on_started",
    ],
    chipset=STM32L476XX,
)
