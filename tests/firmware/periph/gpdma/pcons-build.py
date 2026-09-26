from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "gpdma_ll", [here / "gpdma_ll.c"], entries=["linked", "error", "suspend", "half", "destination"]
)
