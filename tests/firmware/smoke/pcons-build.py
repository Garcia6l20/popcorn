from pathlib import Path

from popcorn_build.firmware import add_firmware

here = Path(__file__).resolve().parent

add_firmware("smoke", "smoke", [here / "smoke.c"])
