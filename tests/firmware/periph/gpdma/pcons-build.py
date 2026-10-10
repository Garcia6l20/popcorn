from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "gpdma_ll",
    [here / "gpdma_ll.c"],
    ["rcc", "spi", "usart"],
    [
        "linked",
        "error",
        "suspend",
        "half",
        "destination",
        "two_d_hw_request",
        "two_d_hw_request_brc1",
    ],
)
