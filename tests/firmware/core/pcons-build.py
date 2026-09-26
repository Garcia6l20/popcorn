from pathlib import Path

from popcorn_build.firmware import add_firmware

here = Path(__file__).resolve().parent

SECTION_BOUNDS = (
    "-Wl,--defsym=_sidata=LOADADDR(.data),--defsym=_sdata=ADDR(.data),"
    "--defsym=_edata=ADDR(.data)+SIZEOF(.data),--defsym=_sbss=ADDR(.bss),"
    "--defsym=_ebss=ADDR(.bss)+SIZEOF(.bss)"
)

UNREFERENCED = {
    "run": ("count_irq",),
    "spin": ("tick_walk", "tick_hang"),
    "periph": ("shared_accesses", "gap_read"),
}

for name, source in (
    ("basic", "basic.c"),
    ("run", "run.c"),
    ("idle", "idle.c"),
    ("calls", "calls.c"),
    ("periph", "periph.c"),
    ("intercepts", "intercepts.c"),
    ("snapshot", "snapshot.c"),
    ("branches", "branches.c"),
    ("spin", "spin.c"),
    ("fnv", "fnv.c"),
):
    _, target = add_firmware("core", name, [here / source])
    if target is not None:
        target.private.link_flags.append(SECTION_BOUNDS)
        target.private.link_flags.extend(
            f"-Wl,--undefined={entry}" for entry in UNREFERENCED.get(name, ())
        )

add_firmware("core", "alu", [here / "alu.S"])
add_firmware("core", "mmio", [here / "mmio.S"])
