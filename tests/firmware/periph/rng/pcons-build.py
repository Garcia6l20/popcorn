from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "rng_ll",
    [here / "rng_ll.c"],
    ["rcc", "rng"],
    ["idle", "enable_read", "nist_init", "locked", "no_kernel_clock", "hsi16_source", "pll_160"],
)
