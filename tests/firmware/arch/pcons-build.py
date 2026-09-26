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
    "svc": (
        "svc_setup",
        "svc_basic",
        "svc_regs",
        "svc_regs_at",
        "svc_psp",
        "svc_psp_top",
        "svc_handler_mode",
        "svc_escalate",
        "svc_escalate_at",
        "svc_unpriv",
        "faultmask_svc",
        "faultmask_svc_after",
        "svc_until",
        "svc_until_after",
        "svc_entry",
        "pendsv_basic",
        "pendsv_tail",
        "pendsv_clear",
        "nmi_run",
        "stir_run",
        "stir_unpriv",
        "stir_allowed",
        "spin_svc",
        "spin_svc_handler",
        "switch_run",
        "task_sp",
        "task_sp_first",
        "control_seen",
        "b_fp_seen",
        "log_buf",
        "log_len",
    ),
    "timers": (
        "timers_setup",
        "tick_1khz",
        "tick_phase",
        "countflag",
        "wake_masked",
        "moved_here",
        "spin_tick",
        "cvr_pair",
        "cyc_pair",
        "cyc_same_block",
        "cyc_frozen",
        "cyc_restart",
        "rvr_race",
        "wfi_empty",
        "wfi_wide",
        "wfi_spot",
        "return_only",
        "wfi_tick",
        "wfi_tick_at",
        "wfe_basic",
        "wfe_sev",
        "wfe_sev_twice",
        "yield_loop",
        "wfe_sevonpend",
        "sleep_on_exit",
        "wfe_primask",
        "wfe_primask_sevonpend",
        "thread_counter",
        "ticks",
        "log_buf",
        "log_len",
    ),
    "faults": (
        "faults_setup",
        "udf",
        "udf_at",
        "udf_forced",
        "udf_forced_at",
        "unmapped_read",
        "unmapped_read_at",
        "unmapped_write",
        "unmapped_write_at",
        "unmapped_fetch",
        "flash_write",
        "flash_write_at",
        "model_bus_error",
        "model_bus_error_next",
        "fault_in_hardfault",
        "faultmask_udf",
        "return_to_reset_lr",
        "bad_exc_return",
        "stack_overflow_entry",
        "bad_vector",
        "invstate",
        "invstate_target",
        "stack_limit",
        "deep_stack_run",
        "unpriv_scs",
        "unpriv_read",
        "spin_udf",
        "log_buf",
        "log_len",
    ),
}

SOURCES = {
    "exceptions": ("exceptions.c", "exceptions.S", "probe.S"),
    "svc": ("svc.c", "svc.S", "switch.S"),
    "timers": ("timers.c", "sleep.S"),
    "faults": ("faults.c", "faults.S"),
}

for name, entries in ENTRIES.items():
    sources = SOURCES.get(name, (f"{name}.c",))
    _, target = add_firmware(
        "arch", name, [here / "startup.c", *(here / source for source in sources)]
    )
    if target is not None:
        target.private.link_flags.append(SECTION_BOUNDS)
        target.private.link_flags.extend(f"-Wl,--undefined={entry}" for entry in entries)
