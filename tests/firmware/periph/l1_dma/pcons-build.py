from pathlib import Path

from popcorn_build.stm32_firmware import STM32L152XB, STM32L152XE, add_ll_firmware

here = Path(__file__).resolve().parent

for chipset in (STM32L152XE, STM32L152XB):
    add_ll_firmware("l1_dma_ll", [here / "l1_dma_ll.c"], [], ["idle"], chipset=chipset)
