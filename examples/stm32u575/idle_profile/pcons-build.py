from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent
firmware = here / "firmware"
sources = [
    firmware / "main.c",
    firmware / "stm32u5xx_it.c",
    firmware / "stm32u5xx_hal_msp.c",
    firmware / "stm32u5xx_hal_timebase_tim.c",
]
hal = ["tim", "tim_ex", "dma", "dma_ex"]

add_example_firmware(here, "idle_wfi", sources, hal=hal, freertos=True, defines=["IDLE_WFI"])
add_example_firmware(here, "idle_spin", sources, hal=hal, freertos=True)
