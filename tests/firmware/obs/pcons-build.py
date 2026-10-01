from pathlib import Path

from popcorn_build.firmware import add_firmware

here = Path(__file__).resolve().parent

add_firmware("obs", "tb", [here / "tb.S"])
add_firmware("obs", "lines", [here / "lines.c"])
_, cut = add_firmware("obs", "cut", [here / "cut.c"])
if cut is not None:
    cut.private.link_flags.extend(f"-Wl,--undefined={name}" for name in ("use_pick", "probe"))
