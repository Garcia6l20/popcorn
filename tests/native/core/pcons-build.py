from pathlib import Path

from pcons import context

from popcorn_build.config import project_dir
from popcorn_build.firmware import firmware
from popcorn_build.testing import add_doctest, alias_bare_name

project = context.current_project
here = Path(__file__).resolve().parent
core = project.get_target("popcorn_core")
smoke_support = project.get_target("popcorn-test-smoke")
test_support = project.get_target("popcorn-test-support")
toy = str(project_dir / "tests" / "data" / "desc" / "toy.json")
core_data = project_dir / "tests" / "data" / "core"

add_doctest("core-types", [here / "test_types.cpp"], [core])
add_doctest("core-fwd", [here / "test_fwd.cpp"], [core])
add_doctest("core-time", [here / "test_time_types.cpp", here / "test_clock.cpp"], [core])
add_doctest("core-services", [here / "test_services.cpp"], [core])
add_doctest("core-errors", [here / "test_errors.cpp"], [core])
add_doctest(
    "core-description",
    [here / "test_description.cpp"],
    [core],
    env={
        "POPCORN_TOY_DESC": toy,
        "POPCORN_DESC_SCHEMA": str(project_dir / "data" / "schema" / "mcu_description.schema.json"),
    },
)
bench_description = project.Program(
    "bench-description", project.default_environment, sources=[here / "bench_description.cpp"]
)
bench_description.link(core)
alias_bare_name(bench_description)
add_doctest(
    "core-machine-api",
    [here / "test_machine_api.cpp", here / "test_trace.cpp", here / "test_snapshot.cpp"],
    [test_support, core],
    env={"POPCORN_TOY_DESC": toy},
)
add_doctest("core-unicorn-dev", [here / "test_unicorn_dev.cpp"], [smoke_support, core])

smoke_elf, smoke_firmware = firmware("smoke", "smoke")
test_smoke_fw = add_doctest(
    "core-smoke-fw",
    [here / "test_smoke_fw.cpp"],
    [smoke_support, core],
    env={"POPCORN_SMOKE_ELF": str(smoke_elf)},
)
if smoke_firmware is not None:
    test_smoke_fw.depends(smoke_firmware)

core_firmware = {name: firmware("core", name) for name in ("basic", "run", "alu", "idle", "calls")}
m33_env = {
    "POPCORN_M33_DESC": str(core_data / "m33.json"),
    "POPCORN_M33_CODE_ALIAS_DESC": str(core_data / "m33_code_alias.json"),
    **{f"POPCORN_FW_{name.upper()}": str(elf) for name, (elf, _) in core_firmware.items()},
}
basic_firmware = core_firmware["basic"][1]
add_doctest("core-memory-map", [here / "test_memory_map.cpp"], [test_support, core], env=m33_env)
test_machine_load = add_doctest(
    "core-machine-load", [here / "test_machine_load.cpp"], [test_support, core], env=m33_env
)
if basic_firmware is not None:
    test_machine_load.depends(basic_firmware)

test_run_loop = add_doctest(
    "core-run-loop", [here / "test_run_loop.cpp"], [test_support, core], env=m33_env
)
test_events = add_doctest(
    "core-events", [here / "test_events_idle.cpp"], [test_support, core], env=m33_env
)
test_calls = add_doctest("core-calls", [here / "test_calls.cpp"], [test_support, core], env=m33_env)
bench_run_loop = project.Program(
    "bench-run-loop", project.default_environment, sources=[here / "bench_run_loop.cpp"]
)
bench_run_loop.link(test_support)
bench_run_loop.link(core)
bench_run_loop.private.defines.extend(
    f'{variable}="{value}"' for variable, value in m33_env.items()
)
alias_bare_name(bench_run_loop)
for _, target in core_firmware.values():
    if target is not None:
        test_run_loop.depends(target)
        test_events.depends(target)
        test_calls.depends(target)
        bench_run_loop.depends(target)
