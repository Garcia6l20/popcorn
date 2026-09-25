from pathlib import Path

from popcorn_build.firmware import add_firmware

here = Path(__file__).resolve().parent

_, basic = add_firmware("core", "basic", [here / "basic.c"])
if basic is not None:
    basic.private.link_flags.append(
        "-Wl,--defsym=_sidata=LOADADDR(.data),--defsym=_sdata=ADDR(.data),"
        "--defsym=_edata=ADDR(.data)+SIZEOF(.data),--defsym=_sbss=ADDR(.bss),"
        "--defsym=_ebss=ADDR(.bss)+SIZEOF(.bss)"
    )
