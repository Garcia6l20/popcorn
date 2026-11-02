from pathlib import Path

from popcorn_build.config import build_dir
from popcorn_build.stm32_firmware import STM32L476XX, add_ll_firmware

here = Path(__file__).resolve().parent

elf, target = add_ll_firmware(
    "l4_smoke",
    [here / "l4_smoke.c"],
    entries=["ram_probe", "sram2_word"],
    chipset=STM32L476XX,
)
if target is not None:
    target.private.link_flags.append(f"-Wl,-Map={elf.relative_to(build_dir).with_suffix('.map')}")
