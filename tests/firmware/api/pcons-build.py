from pathlib import Path

from popcorn_build.firmware import add_firmware

here = Path(__file__).resolve().parent

VECTOR_TABLES = ("vt_idle", "vt_fault", "vt_reset", "vt_spin", "vt_bkpt")
SECTION_BOUNDS = (
    "-Wl,--defsym=_sidata=LOADADDR(.data),--defsym=_sdata=ADDR(.data),"
    "--defsym=_edata=ADDR(.data)+SIZEOF(.data),--defsym=_sbss=ADDR(.bss),"
    "--defsym=_ebss=ADDR(.bss)+SIZEOF(.bss)"
)

_, driver = add_firmware("api", "driver", [here / "driver.c"])
if driver is not None:
    driver.private.link_flags.extend(f"-Wl,--undefined={table}" for table in VECTOR_TABLES)

_, views = add_firmware("api", "views", [here / "views.c"])
if views is not None:
    kept = ("table", "matrix", "banner", "extras")
    views.private.link_flags.extend([SECTION_BOUNDS, *(f"-Wl,--undefined={name}" for name in kept)])

CALLED = (
    "add",
    "negate",
    "ones",
    "twice64",
    "mulf",
    "half",
    "sum6",
    "fill",
    "mixed",
    "by_value",
    "low_byte",
    "neg16",
    "soft_mixed",
    "stack_now",
    "control_now",
    "bump",
    "forever",
    "fault",
    "wait_flag",
)
_, callee = add_firmware("api", "callee", [here / "callee.c"])
if callee is not None:
    callee.private.link_flags.extend(
        [SECTION_BOUNDS, *(f"-Wl,--undefined={name}" for name in CALLED)]
    )
    callee.private.link_libs.append("gcc")
