from pcons import Project, get_var

from popcorn_build.config import POPCORN_VERSION, VARIANT, build_dir, project_dir, toolchain
from popcorn_build.firmware import add_firmware
from popcorn_build.python import add_extension, add_stub
from popcorn_build.requirements import package, unicorn_commit
from popcorn_build.testing import add_doctest

WARNINGS = [
    "-Wall",
    "-Wextra",
    "-Wreturn-type",
    "-Wdangling-reference",
    "-Wformat=2",
    "-Wimplicit-fallthrough",
    "-Wnon-virtual-dtor",
    "-Woverloaded-virtual",
    "-Wcast-qual",
    "-Wstrict-aliasing=2",
    "-Wdouble-promotion",
    "-Werror",
]

ABI_VERSION = 21

project = Project("popcorn", root_dir=project_dir, build_dir=build_dir)
env = project.Environment(toolchain=toolchain)
env.cxx.flags.extend(
    [
        "-std=gnu++26",
        "-freflection",
        "-fimplicit-constexpr",
        f"-Wabi={ABI_VERSION}",
        f"-fabi-version={ABI_VERSION}",
        "-fmax-errors=5",
        "-fPIC",
        *WARNINGS,
    ]
)

if VARIANT == "asan":
    env.set_variant("debug")
    env.apply_preset("sanitize")
else:
    env.set_variant(VARIANT)

core = project.StaticLibrary(
    "popcorn_core",
    env,
    sources=[
        "native/src/core/version.cpp",
        "native/src/core/engine.cpp",
        "native/src/core/smoke.cpp",
    ],
)
core.public.include_dirs.append("native/include")
core.private.defines.extend(
    [f'POPCORN_VERSION="{POPCORN_VERSION}"', f'POPCORN_UNICORN_COMMIT="{unicorn_commit[:12]}"']
)
core.link(package("unicorn"))

add_doctest("core-engine", ["tests/native/core/test_engine.cpp"], [core])

smoke_elf, smoke_firmware = add_firmware("smoke", "smoke", ["tests/firmware/smoke/smoke.c"])
test_smoke_fw = add_doctest(
    "core-smoke-fw",
    ["tests/native/core/test_smoke_fw.cpp"],
    [core],
    env={"POPCORN_SMOKE_ELF": str(smoke_elf)},
)
if smoke_firmware is not None:
    test_smoke_fw.depends(smoke_firmware)

if VARIANT != "asan":
    ext = add_extension("popcorn", "_core", ["native/src/bind/module.cpp"])
    ext.link(core)

    python_package = project_dir / "python" / "popcorn"
    python_sources = [*sorted(python_package.rglob("*.py")), python_package / "py.typed"]
    staged_sources = project.Install("popcorn", python_sources, name="stage-python", no_prefix=True)
    staged_sources.build_tier = "default"
    stub = add_stub(ext, "popcorn", "_core", after=[staged_sources])

    if get_var("PCONS_BUILD_WHEEL", False):
        project.Alias(
            "wheel", project.Install("popcorn", [ext, stub, *python_sources], name="install")
        )
