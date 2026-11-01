from pathlib import Path

from popcorn_build.config import build_dir
from popcorn_build.stm32_firmware import STM32L152XB, STM32L152XC, STM32L152XE, add_ll_firmware

here = Path(__file__).resolve().parent

for chipset in (STM32L152XE, STM32L152XB, STM32L152XC):
    elf, target = add_ll_firmware(
        "l1_smoke", [here / "l1_smoke.c"], entries=["ram_probe"], chipset=chipset
    )
    if target is not None:
        target.private.link_flags.append(
            f"-Wl,-Map={elf.relative_to(build_dir).with_suffix('.map')}"
        )
