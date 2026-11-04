from pathlib import Path

from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware("l4_dma_ll", [here / "l4_dma_ll.c"], ["dma"], ["idle"], chipset=STM32L476XX)
