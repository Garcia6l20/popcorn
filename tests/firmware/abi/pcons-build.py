from pathlib import Path

from popcorn_build.firmware import add_firmware

here = Path(__file__).resolve().parent
arch = here.parent / "arch"

SECTION_BOUNDS = (
    "-Wl,--defsym=_sidata=LOADADDR(.data),--defsym=_sdata=ADDR(.data),"
    "--defsym=_edata=ADDR(.data)+SIZEOF(.data),--defsym=_sbss=ADDR(.bss),"
    "--defsym=_ebss=ADDR(.bss)+SIZEOF(.bss)"
)

ENTRIES = (
    "add",
    "mul64",
    "add64_after3",
    "fadd",
    "dmul",
    "backfill",
    "sum10",
    "d9",
    "mixed",
    "soft_fadd",
    "soft_dmul",
    "soft_mixed",
    "struct_sum",
    "read_control",
    "try_cpsid",
    "set_basepri",
    "read_sp_state",
    "svc_in_call",
    "spin_forever",
    "reprogram_systick",
    "call_mixed",
    "call_soft_mixed",
    "main_loop",
    "main_wfi",
    "mpu_setup",
    "unpriv_loop",
    "handler_pause",
    "poll_stat",
    "SysTick_Handler",
    "ticks",
    "counter",
    "svc_value",
    "irq3_count",
    "sp_result",
    "mixed_seen",
    "log_buf",
    "log_len",
)

_, target = add_firmware("abi", "abi", [arch / "startup.c", here / "abi.c"])
if target is not None:
    target.private.link_flags.append(SECTION_BOUNDS)
    target.private.link_flags.extend(f"-Wl,--undefined={entry}" for entry in ENTRIES)
    target.private.link_libs.append("gcc")
