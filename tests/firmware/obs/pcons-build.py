from pathlib import Path

from popcorn_build.firmware import add_firmware

here = Path(__file__).resolve().parent

add_firmware("obs", "tb", [here / "tb.S"])
add_firmware("obs", "lines", [here / "lines.c"])
_, cut = add_firmware("obs", "cut", [here / "cut.c"])
if cut is not None:
    cut.private.link_flags.extend(f"-Wl,--undefined={name}" for name in ("use_pick", "probe"))
add_firmware("obs", "shared_exit", [here / "shared_exit.S"])
_, shapes = add_firmware("obs", "genhtml_shapes", [here / "genhtml_shapes.c"])
if shapes is not None:
    shapes.private.link_flags.extend(
        f"-Wl,--undefined={name}" for name in ("inline_f", "alias_two", "same_b")
    )

_, unmodelled = add_firmware("obs", "unmodelled", [here / "unmodelled.c"])
if unmodelled is not None:
    unmodelled.private.link_flags.extend(
        f"-Wl,--undefined={name}"
        for name in (
            "poll_unmodelled",
            "touch_stat",
            "read_unimplemented",
            "read_no_register",
            "read_modelled",
            "boot_done",
        )
    )
