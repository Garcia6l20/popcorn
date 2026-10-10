from pathlib import Path

from pcons import context

from popcorn_build.config import project_dir
from popcorn_build.firmware import firmware
from popcorn_build.testing import add_doctest, alias_bare_name

here = Path(__file__).resolve().parent
project = context.current_project
core = project.get_target("popcorn_core")
test_support = project.get_target("popcorn-test-support")
core_data = project_dir / "tests" / "data" / "core"

add_doctest("arch-interface", [here / "test_interface.cpp"], [core])
add_doctest("arch-thumb", [here / "test_thumb.cpp"], [core])

arch_firmware = {
    name: firmware("arch", name)
    for name in ("scs", "exceptions", "svc", "timers", "faults", "rtos")
}
arch_firmware["abi"] = firmware("abi", "abi")
arch_firmware["rom_store"] = firmware("core", "rom_store")
arch_firmware["irq"] = firmware("core", "irq")
arch_firmware["isr"] = firmware("api", "isr")
for variant in (
    "exceptions_m4",
    "exceptions_m7",
    "exceptions_m3",
    "svc_m4",
    "timers_m4",
    "faults_m4",
):
    arch_firmware[variant] = firmware("arch", variant)
arch_firmware["abi_m4"] = firmware("abi", "abi_m4")
arch_env = {
    "POPCORN_M33_DESC": str(core_data / "m33.json"),
    **{f"POPCORN_FW_{name.upper()}": str(elf) for name, (elf, _) in arch_firmware.items()},
}

private_dir = project_dir / "native" / "src" / "arch" / "armv8m"
arch_tests = [
    add_doctest(name, [here / source], [test_support, core], env=arch_env)
    for name, source in (
        ("arch-scs", "test_scs.cpp"),
        ("arch-exceptions", "test_exceptions.cpp"),
        ("arch-units", "test_units.cpp"),
        ("arch-svc", "test_svc.cpp"),
        ("arch-timers", "test_timers.cpp"),
        ("arch-faults", "test_faults.cpp"),
        ("arch-calls", "test_calls.cpp"),
        ("arch-variants", "test_variants.cpp"),
        ("arch-rtos", "test_rtos.cpp"),
        ("arch-store-controller", "test_store_controller.cpp"),
        ("arch-scs-exit", "test_scs_exit.cpp"),
    )
]
for test in arch_tests:
    test.private.include_dirs.append(private_dir)
    for _, target in arch_firmware.values():
        if target is not None:
            test.depends(target)

bench_arch = project.Program(
    "bench-arch", project.default_environment, sources=[here / "bench_arch.cpp"]
)
bench_arch.link(test_support)
bench_arch.link(core)
bench_arch.private.include_dirs.append(private_dir)
bench_arch.private.defines.extend(
    f'{variable}="{value}"'
    for variable, value in arch_env.items()
    if variable in ("POPCORN_M33_DESC", "POPCORN_FW_RTOS")
)
alias_bare_name(bench_arch)
_, rtos_target = arch_firmware["rtos"]
if rtos_target is not None:
    bench_arch.depends(rtos_target)
