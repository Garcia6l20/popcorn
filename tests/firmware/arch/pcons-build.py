from pathlib import Path

from popcorn_build.firmware import add_firmware

here = Path(__file__).resolve().parent

SECTION_BOUNDS = (
    "-Wl,--defsym=_sidata=LOADADDR(.data),--defsym=_sdata=ADDR(.data),"
    "--defsym=_edata=ADDR(.data)+SIZEOF(.data),--defsym=_sbss=ADDR(.bss),"
    "--defsym=_ebss=ADDR(.bss)+SIZEOF(.bss)"
)

ENTRIES = {
    "scs": (
        "scs_run",
        "reset_req",
        "reset_req_wrong_key",
        "unpriv_psp",
        "msplim_set",
        "deep_stack",
        "log_buf",
        "log_len",
    ),
}

for name, entries in ENTRIES.items():
    _, target = add_firmware("arch", name, [here / "startup.c", here / f"{name}.c"])
    if target is not None:
        target.private.link_flags.append(SECTION_BOUNDS)
        target.private.link_flags.extend(f"-Wl,--undefined={entry}" for entry in entries)
