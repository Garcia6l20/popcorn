from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware("adc_ll", [here / "adc_ll.c"], [], ["idle", "scan_awd"])
