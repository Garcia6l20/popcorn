from pcons import Project

from popcorn_build.config import POPCORN_VERSION, VARIANT, build_dir, project_dir, toolchain
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
    sources=["native/src/core/version.cpp", "native/src/core/engine.cpp"],
)
core.public.include_dirs.append("native/include")
core.private.defines.extend(
    [f'POPCORN_VERSION="{POPCORN_VERSION}"', f'POPCORN_UNICORN_COMMIT="{unicorn_commit[:12]}"']
)
core.link(package("unicorn"))

add_doctest("core-engine", ["tests/native/core/test_engine.cpp"], [core])
