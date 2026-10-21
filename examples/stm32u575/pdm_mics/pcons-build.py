from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent
firmware = here / "firmware"

add_example_firmware(
    here,
    "pdm_mics",
    [firmware / "main.c", firmware / "stm32u5xx_it.c", firmware / "stm32u5xx_hal_msp.c"],
    hal=["mdf", "dma", "dma_ex", "rcc_ex"],
)
