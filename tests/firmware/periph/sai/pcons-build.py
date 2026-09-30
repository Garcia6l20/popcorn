from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware("sai_ll", [here / "sai_ll.c"], [], ["idle", "tx_poll", "duplex_irq", "underrun"])
