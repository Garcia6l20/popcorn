from pcons import Project, add_subdirectory, get_var

from popcorn_build import check, schema
from popcorn_build.config import POPCORN_VERSION, VARIANT, build_dir, gcc_ar, project_dir, toolchain
from popcorn_build.options import build_examples, build_tests
from popcorn_build.python import (
    add_extension,
    add_package_install,
    add_stub,
)
from popcorn_build.requirements import package, unicorn_commit
from popcorn_build.testing import add_header_check

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
        "-fcontracts",
        f"-Wabi={ABI_VERSION}",
        f"-fabi-version={ABI_VERSION}",
        "-fmax-errors=5",
        "-fPIC",
        *WARNINGS,
    ]
)
env.link.flags.append("-fcontracts")

if VARIANT == "asan":
    env.set_variant("debug")
    env.apply_preset("sanitize")
else:
    env.set_variant(VARIANT)
    env.ar.cmd = gcc_ar

CORE_AREAS = ("core", "arch", "periph", "image")

core = project.StaticLibrary(
    "popcorn_core",
    env,
    sources=[
        source for area in CORE_AREAS for source in add_subdirectory(f"native/src/{area}").sources
    ],
)
core.public.include_dirs.append("native/include")
core.private.defines.extend(
    [f'POPCORN_VERSION="{POPCORN_VERSION}"', f'POPCORN_UNICORN_COMMIT="{unicorn_commit}"']
)
core.link(package("unicorn"))
core.link(package("reflex-serde"))
if VARIANT != "asan":
    core.private.compile_flags.append("-flto=auto")
    core.public.link_flags.extend(["-flto=2", "-Werror=lto-type-mismatch", "-Werror=odr"])

obs = add_subdirectory("native/src/obs")

popcorn_config = project.StaticLibrary(
    "popcorn_config", env, sources=add_subdirectory("native/src/config").sources
)
popcorn_config.link(core)

describe_schema = project.Program(
    "popcorn-describe-schema", env, sources=["native/src/tools/describe_schema.cpp"]
)
describe_schema.link(core)
regs_gen = project.Program("popcorn-regs-gen", env, sources=["native/src/tools/regs_gen.cpp"])
regs_gen.link(core)

if build_tests:
    add_header_check("headers", project_dir / "native" / "include", [core])
    smoke_support = add_subdirectory("tests/native/support").smoke_support
    for tree in ("tests/firmware", "tests/native"):
        for script in sorted((project_dir / tree).glob("*/pcons-build.py")):
            if script.parent.name != "support":
                add_subdirectory(script.parent.relative_to(project_dir))
if build_examples:
    for script in sorted((project_dir / "examples").glob("*/*/pcons-build.py")):
        add_subdirectory(script.parent.relative_to(project_dir))

if VARIANT != "asan":
    ext = add_extension("popcorn", "_core", add_subdirectory("native/src/bind").sources)
    ext.link(core)
    ext.link(obs.popcorn_obs)
    ext.link(popcorn_config)
    if build_tests:
        smoke_ext = add_extension(
            "popcorn_testing", "_smoke", ["tests/native/support/smoke_module.cpp"]
        )
        smoke_ext.link(smoke_support)
        smoke_ext.link(core)
        add_stub(smoke_ext, "popcorn_testing", "_smoke")

    python_package = project_dir / "python" / "popcorn"
    staged_sources = add_package_install(
        "popcorn", python_package, name="stage-python", no_prefix=True
    )
    for staged in staged_sources:
        staged.build_tier = "default"
    project.Alias("stage-python", *staged_sources)
    regs_path = build_dir / "popcorn" / "arm" / "_regs.py"
    regs = project.Command(
        "popcorn.arm._regs",
        env,
        target=regs_path,
        command=[str(build_dir / "popcorn-regs-gen"), str(regs_path)],
    )
    regs.depends(regs_gen)
    regs.build_tier = "default"
    obs_python = obs.add_python([*staged_sources, regs, ext])
    stub = add_stub(ext, "popcorn", "_core", after=[*staged_sources, regs, obs_python[0]])

    if get_var("PCONS_BUILD_WHEEL", False):
        project.Alias(
            "wheel",
            project.Install("popcorn", [ext, stub, *obs_python], name="install"),
            project.InstallAs("popcorn/arm/_regs.py", regs, name="install-regs"),
            *add_package_install("popcorn", python_package, name="install"),
        )

check.register()
schema.register()
