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
    "exceptions": (
        "exceptions_run",
        "probe_setup",
        "latch_pulse",
        "latch_held",
        "it_irq_label",
        "regs_thread_msp",
        "regs_thread_msp_fp",
        "regs_thread_psp",
        "regs_thread_psp_fp",
        "regs_thread_psp_unpriv",
        "regs_misaligned",
        "regs_nested",
        "regs_nested_fp",
        "tail_chain",
        "faultmask_to_unpriv",
        "faultmask_seen",
        "icsr_probe",
        "icsr_seen",
        "log_buf",
        "log_len",
    ),
}

SOURCES = {"exceptions": ("exceptions.c", "exceptions.S", "probe.S")}

for name, entries in ENTRIES.items():
    sources = SOURCES.get(name, (f"{name}.c",))
    _, target = add_firmware(
        "arch", name, [here / "startup.c", *(here / source for source in sources)]
    )
    if target is not None:
        target.private.link_flags.append(SECTION_BOUNDS)
        target.private.link_flags.extend(f"-Wl,--undefined={entry}" for entry in entries)
