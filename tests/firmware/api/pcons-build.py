from pathlib import Path

from popcorn_build.firmware import add_firmware

here = Path(__file__).resolve().parent

VECTOR_TABLES = ("vt_idle", "vt_fault", "vt_reset", "vt_spin", "vt_bkpt")

_, target = add_firmware("api", "driver", [here / "driver.c"])
if target is not None:
    target.private.link_flags.extend(f"-Wl,--undefined={table}" for table in VECTOR_TABLES)
