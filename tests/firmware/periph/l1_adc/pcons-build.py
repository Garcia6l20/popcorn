from pathlib import Path

from popcorn_build.stm32_firmware import STM32L152XB, STM32L152XE, add_ll_firmware

here = Path(__file__).resolve().parent

for chipset in (STM32L152XE, STM32L152XB):
    _, target = add_ll_firmware(
        "l1_adc_ll",
        [here / "l1_adc_ll.c"],
        entries=["idle", "single", "scan3", "triggered", "injected", "dma3", "watchdog", "factory"],
        chipset=chipset,
    )
    if target is not None:
        target.private.compile_flags.append("-Wno-unused-parameter")
