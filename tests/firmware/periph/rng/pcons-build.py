from pathlib import Path

from popcorn_build.stm32_firmware import add_ll_firmware

here = Path(__file__).resolve().parent

add_ll_firmware(
    "rng_ll",
    [here / "rng_ll.c"],
    ["rcc", "rng"],
    [
        "idle",
        "enable_read",
        "enable_read_idle",
        "nist_init",
        "locked",
        "no_kernel_clock",
        "hsi16_source",
        "pll_160",
        "seed_recover_condrst",
        "seed_recover_auto",
        "clock_error",
        "clock_error_ced_off",
        "keygen",
        "keygen_status",
        "keygen_done",
        "keygen_key",
        "checkpoint",
    ],
)
