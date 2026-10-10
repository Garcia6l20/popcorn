from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "tim_ll",
    [here / "tim_ll.c"],
    ["rcc", "tim"],
    [
        "idle",
        "tim16_updates",
        "tim16_one_pulse",
        "tim2_compare",
        "tim6_preload",
        "tim6_clock",
        "tim6_dma",
        "tim6_poll",
        "tim4_triggers_tim5",
        "tim1_clocks_tim15",
        "tim15_unclocked",
        "tim3_encoder_idle",
    ],
)
