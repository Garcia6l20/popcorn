from pathlib import Path

from popcorn_build.firmware import add_firmware

here = Path(__file__).resolve().parent

VECTOR_TABLES = ("vt_idle", "vt_fault", "vt_reset", "vt_spin", "vt_bkpt", "vt_jump")
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
    kept = ("table", "matrix", "banner", "extras", "wide", "odd", "armed", "shadowed")
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
    "ticks_now",
    "apply",
    "negated",
    "scaled",
    "halved",
    "touched",
    "depth",
    "use_pair",
)
_, callee = add_firmware("api", "callee", [here / "callee.c"])
if callee is not None:
    callee.private.link_flags.extend(
        [SECTION_BOUNDS, *(f"-Wl,--undefined={name}" for name in CALLED)]
    )
    callee.private.link_libs.append("gcc")

HOOKED = (
    "use_delay",
    "run_script",
    "use_triple",
    "write_three",
    "use_libc",
    "note_then_fault",
    "use_fact",
    "use_pair",
    "use_strlen_ff",
)
_, hooks = add_firmware("api", "hooks", [here / "hooks.c"])
if hooks is not None:
    hooks.private.link_flags.extend(
        [SECTION_BOUNDS, *(f"-Wl,--undefined={name}" for name in HOOKED)]
    )

MODELLED = (
    "start_timer",
    "stop_timer",
    "wait_timer_irqs",
    "eeprom_roundtrip",
    "read_timer_count",
    "read_timer_count_n",
    "write_timer_cr",
    "touch_unmodelled",
    "write_then_fault",
    "ready",
)
_, model = add_firmware("api", "model", [here / "model.c"])
if model is not None:
    model.private.link_flags.extend(
        [SECTION_BOUNDS, *(f"-Wl,--undefined={name}" for name in MODELLED)]
    )

_, board = add_firmware("api", "board", [here / "board.c"])
if board is not None:
    board.private.link_flags.extend([SECTION_BOUNDS, "-Wl,--undefined=ready"])

FAULTING = (
    "level1",
    "level2",
    "level3",
    "in_handler",
    "ready",
    "worker",
    "vt_nested",
    "null_call",
)
_, fault = add_firmware("api", "fault", [here / "fault.c"])
if fault is not None:
    fault.private.link_flags.extend(
        [SECTION_BOUNDS, *(f"-Wl,--undefined={name}" for name in FAULTING)]
    )

_, consts = add_firmware("api", "consts", [here / "consts_a.c", here / "consts_b.c"])
if consts is not None:
    consts.private.compile_flags.append("-g3")
    consts.private.link_flags.append("-Wl,--undefined=channel")

_, isr = add_firmware("api", "isr", [here / "isr.c"])
if isr is not None:
    isr.private.link_flags.append(SECTION_BOUNDS)
