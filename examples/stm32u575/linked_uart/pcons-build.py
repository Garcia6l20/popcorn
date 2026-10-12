from pathlib import Path

from popcorn_build.examples import add_example_firmware

here = Path(__file__).resolve().parent
firmware = here / "firmware"

for name in ("controller", "responder"):
    add_example_firmware(
        here,
        f"linked_{name}",
        [firmware / f"{name}.c", firmware / "link.c"],
        hal=["uart", "uart_ex", "dma", "dma_ex", "iwdg"],
    )
