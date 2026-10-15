from pathlib import Path

from pcons import Target, context

from popcorn_build.config import VARIANT, build_dir, project_dir
from popcorn_build.python import add_extension
from popcorn_build.testing import add_doctest, popcorn_libraries

here = Path(__file__).resolve().parent
project = context.current_project
fixture_dir = build_dir / "tests" / "native" / "plugin" / "plugin-fixtures"
core, stm32 = popcorn_libraries("core", "stm32")
(core_static,) = popcorn_libraries("core", static=True)
CASE_TIMEOUT = 60.0


def plugin_fixture(name: str, links: list[Target]) -> Target:
    """The fixture plugin @p name, fixtures/@p name.cpp built as plugin-fixtures/@p name.so."""
    library = project.SharedLibrary(
        f"plugin-fixture-{name}",
        project.default_environment,
        sources=[here / "fixtures" / f"{name}.cpp"],
    )
    library.output_prefix = "plugin-fixtures/"
    library.output_name = name
    library.output_suffix = ".so"
    library.private.compile_flags.append("-fvisibility=hidden")
    library.private.link_flags.extend(
        [f"-Wl,-rpath,{build_dir / 'popcorn'}", f"-Wl,-rpath,{fixture_dir}", "-Wl,-z,defs"]
    )
    for linked in links:
        library.link(linked)
    return library


good = plugin_fixture("good", [core])
stripped_path = fixture_dir / "good-stripped.so"
stripped = project.Command(
    project.default_environment,
    name="plugin-fixture-good-stripped",
    target=stripped_path,
    command=["strip", "--strip-unneeded", "-o", str(stripped_path), str(fixture_dir / "good.so")],
)
stripped.depends(good)

core_fixtures = [
    good,
    stripped,
    *(
        plugin_fixture(name, [core])
        for name in (
            "mismatch",
            "sdk_mismatch",
            "ctor_aborts",
            "no_init",
            "throws",
            "dup_name",
            "writes_global",
            "writes_arch",
            "ctor_registers",
            "init_swallows",
            "nested_load",
        )
    ),
]
glibcxx_debug = plugin_fixture("glibcxx_debug", [core])
glibcxx_debug.private.compile_flags.append("-D_GLIBCXX_DEBUG")
core_fixtures.append(glibcxx_debug)
fixture_env = {
    "POPCORN_PLUGIN_FIXTURES": str(fixture_dir),
    "POPCORN_PLUGIN_CORE": str(build_dir / "popcorn" / "libpopcorn_core.so"),
}

records = add_doctest(
    "plugin-records", [here / "test_records.cpp"], [core], env=fixture_env, timeout=CASE_TIMEOUT
)
records.depends(good)

loader = add_doctest(
    "plugin-loader", [here / "test_loader.cpp"], [core], env=fixture_env, timeout=CASE_TIMEOUT
)
loader.depends(*core_fixtures, plugin_fixture("no_init_stm32", [stm32, good, core]))

elf_scan = add_doctest(
    "plugin-elf-scan",
    [here / "test_elf_scan.cpp"],
    [core_static],
    env=fixture_env,
    timeout=CASE_TIMEOUT,
)
elf_scan.private.include_dirs.append(project_dir / "native" / "src" / "plugin")
elf_scan.depends(good)

if VARIANT not in ("asan", "tsan"):
    add_extension("popcorn_testing", "_plugin_bindings_check", [here / "bindings_check.cpp"]).link(
        core
    )
