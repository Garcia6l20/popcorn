import json
import shutil
import tomllib
from collections.abc import Sequence
from pathlib import Path

from pcons import Project, Target, add_subdirectory, get_var

from popcorn_build import check, schema
from popcorn_build.config import POPCORN_VERSION, VARIANT, build_dir, gcc_ar, project_dir, toolchain
from popcorn_build.layering import misplaced_sources
from popcorn_build.options import build_examples, build_tests
from popcorn_build.plugin_sdk import SDK, sdk_headers
from popcorn_build.python import (
    add_extension,
    add_package_install,
    add_stub,
    ext_suffix,
    interpreter,
    nanobind_runtime,
)
from popcorn_build.requirements import package, prefixes, unicorn_archive, unicorn_commit
from popcorn_build.sdk import reflex_include_dirs, sdk_inputs
from popcorn_build.testing import add_header_check, check_core_links

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

if get_var("POPCORN_ENABLE_CCACHE", False):
    env.use_compiler_cache()

if VARIANT == "asan":
    env.set_variant("debug")
    env.cxx.flags.extend(["-fsanitize=address", "-fno-omit-frame-pointer"])
    env.link.flags.append("-fsanitize=address")
elif VARIANT == "tsan":
    env.set_variant("debug")
    env.cxx.flags.append("-fsanitize=thread")
    env.link.flags.append("-fsanitize=thread")
else:
    env.set_variant(VARIANT)
    env.ar.cmd = gcc_ar

building_wheel = get_var("PCONS_BUILD_WHEEL", False)
lto = get_var("POPCORN_ENABLE_LTO", building_wheel) and VARIANT not in ("asan", "tsan")
LTO_LINK_FLAGS = ["-flto=2", "-Werror=lto-type-mismatch", "-Werror=odr"]

reflex_root = prefixes["reflex"]
nanobind_version = tomllib.loads((project_dir / "deps.toml").read_text())["packages"]["nanobind"][
    "version"
]
generated_include = build_dir / "generated" / "include"
build_info_path = generated_include / "popcorn" / "plugin" / "build_info.hpp"
native_include = project_dir / "native" / "include"
build_info = project.Command(
    env,
    name="popcorn-build-info",
    target=build_info_path,
    source=sdk_inputs(native_include, reflex_include_dirs(reflex_root)),
    command=[
        interpreter,
        "-m",
        "popcorn_build.sdk",
        str(build_info_path),
        POPCORN_VERSION,
        nanobind_version,
        VARIANT,
        str(native_include),
        str(reflex_root),
    ],
    cwd=project_dir,
    restat=True,
)

core_headers = project.HeaderOnlyLibrary(
    "popcorn_core_headers", include_dirs=["native/include", str(generated_include)]
)
core_headers.depends(build_info)
core_headers.public.compile_flags.append("-fno-plt")
core_headers.link(package("unicorn-headers"))
core_headers.link(package("reflex-serde"))

periph = add_subdirectory("native/src/periph")
arch = add_subdirectory("native/src/arch")
obs = add_subdirectory("native/src/obs")
plugin = add_subdirectory("native/src/plugin")


owned: dict[str, list[Path]] = {}


def area_objects(name: str, sources: Sequence[Path], lto: bool) -> Target:
    """The object library @p name of an area library, hidden and, with @p lto, LTO'd."""
    objects = project.ObjectLibrary(name, env, sources=list(sources))
    objects.link(core_headers)
    objects.private.compile_flags.extend(["-fvisibility=hidden", "-fno-semantic-interposition"])
    if lto:
        objects.private.compile_flags.append("-flto=auto")
    return objects


def area_library(
    name: str,
    sources: Sequence[Path],
    links: Sequence[tuple[Target, Target]] = (),
    entry: Path | None = None,
) -> tuple[Target, Target, Target]:
    """Objects, shared libpopcorn_<name>.so in build/popcorn and its static twin.

    Each library links the same flavour of every pair in @p links, shared to shared, static to
    static, so a program reaches one flavour only. The static twin serves white-box tests and
    the build-time tools. @p entry, the built-in plugin entry point, goes into the shared
    library only, the static twin has no entry. A duplicate in two archives does not fail a
    static link, nothing pulls the members, so only the archive check catches it.
    """
    owned[name] = [*sources, *([entry] if entry is not None else [])]
    objects = area_objects(f"popcorn_{name}_objects", sources, lto)
    shared_sources = [objects]
    if entry is not None:
        shared_sources.append(area_objects(f"popcorn_{name}_entry", [entry], lto))
    shared = project.SharedLibrary(f"popcorn_{name}", env, sources=shared_sources)
    shared.output_prefix = "popcorn/lib"
    shared.link(core_headers)
    shared.private.link_flags.extend(
        [f"-Wl,-soname,libpopcorn_{name}.so", "-Wl,-z,defs", "-Wl,-rpath,$$ORIGIN"]
    )
    static = project.StaticLibrary(f"popcorn_{name}_static", env, sources=[objects])
    static.link(core_headers)
    for linked_shared, linked_static in links:
        shared.link(linked_shared)
        static.link(linked_static)
    if lto:
        shared.private.link_flags.extend(LTO_LINK_FLAGS)
        static.public.link_flags.extend(LTO_LINK_FLAGS)
    return objects, shared, static


core_objects, core, core_static = area_library(
    "core",
    [
        *add_subdirectory("native/src/core").sources,
        *add_subdirectory("native/src/image").sources,
        *arch.generic,
        *periph.framework,
        *plugin.sources,
    ],
)
core_objects.private.defines.extend(
    [f'POPCORN_VERSION="{POPCORN_VERSION}"', f'POPCORN_UNICORN_COMMIT="{unicorn_commit}"']
)
core.link_private(package("unicorn")).depends(unicorn_archive)
core_version_script = project_dir / "native" / "src" / "core" / "popcorn_core.map"
core.private.link_flags.append(f"-Wl,--version-script={core_version_script}")
core.depends(core_version_script)
core_static.link(package("unicorn")).depends(unicorn_archive)

_, armv8m, armv8m_static = area_library(
    "armv8m", arch.armv8m, [(core, core_static)], arch.armv8m_entry
)
_, usb, usb_static = area_library("usb", periph.usb, [(core, core_static)])
_, stm32, stm32_static = area_library(
    "stm32", periph.stm32, [(usb, usb_static), (core, core_static)], periph.stm32_entry
)
_, devices, devices_static = area_library("devices", periph.devices, [(core, core_static)])
_, popcorn_obs, popcorn_obs_static = area_library("obs", obs.sources, [(core, core_static)])

shared_libraries = [core, armv8m, usb, stm32, devices, popcorn_obs]
static_libraries = [core_static, armv8m_static, usb_static, stm32_static, devices_static]
static_libraries.append(popcorn_obs_static)

tool_sources = {
    "popcorn-describe-schema": project_dir / "native" / "src" / "tools" / "describe_schema.cpp",
    "popcorn-regs-gen": project_dir / "native" / "src" / "tools" / "regs_gen.cpp",
}
bind_sources = add_subdirectory("native/src/bind").sources
misplaced = misplaced_sources(
    project_dir / "native",
    {
        **owned,
        "_core": bind_sources,
        "_obs": obs.extension_sources,
        "popcorn.devices._native": periph.devices_extension,
        **{tool: [source] for tool, source in tool_sources.items()},
    },
)
if misplaced:
    raise SystemExit(f"popcorn: each native source belongs to one library: {'; '.join(misplaced)}")

describe_schema = project.Program(
    "popcorn-describe-schema", env, sources=[tool_sources["popcorn-describe-schema"]]
)
describe_schema.link(core_static)
regs_gen = project.Program("popcorn-regs-gen", env, sources=[tool_sources["popcorn-regs-gen"]])
regs_gen.link(armv8m_static)
regs_gen.link(core_static)

if build_tests:
    add_header_check(
        "headers",
        project_dir / "native" / "include",
        shared_libraries,
        exclude=["popcorn/plugin/bindings.hpp"],
    )
    smoke_support = add_subdirectory("tests/native/support").smoke_support
    for tree in ("tests/firmware", "tests/native"):
        for script in sorted((project_dir / tree).glob("*/pcons-build.py")):
            if script.parent.name != "support":
                add_subdirectory(script.parent.relative_to(project_dir))
if build_examples:
    for script in sorted((project_dir / "examples").glob("*/*/pcons-build.py")):
        add_subdirectory(script.parent.relative_to(project_dir))

if VARIANT not in ("asan", "tsan"):
    ext = add_extension("popcorn", "_core", bind_sources)
    for library in shared_libraries:
        if library is not armv8m and library is not devices:
            ext.link(library)
    ext.depends(armv8m)
    devices_ext = add_extension("popcorn.devices", "_native", periph.devices_extension)
    devices_ext.link(devices)
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
        env,
        name="popcorn.arm._regs",
        target=regs_path,
        command=[str(build_dir / "popcorn-regs-gen"), str(regs_path)],
    )
    regs.depends(regs_gen)
    regs.build_tier = "default"
    obs_python = obs.add_python(popcorn_obs, [*staged_sources, regs, ext, devices_ext])
    stub = add_stub(
        ext,
        "popcorn",
        "_core",
        after=[*staged_sources, regs, obs_python[0], devices_ext],
        include_private=True,
    )
    devices_stub = add_stub(
        devices_ext, "popcorn.devices", "_native", after=[stub], include_private=True
    )
    sdk_json_path = build_dir / SDK / "sdk.json"
    sdk_compiler = shutil.which(env.cxx.cmd) or env.cxx.cmd
    sdk_fields = {
        "version": POPCORN_VERSION,
        "variant": VARIANT,
        "compiler": sdk_compiler,
        "cxx_flags": [
            *(flag for flag in env.cxx.flags if flag not in WARNINGS and "max-errors" not in flag),
            "-fno-plt",
        ],
        "defines": list(env.cxx.defines),
        "link_flags": list(env.link.flags),
        "ext_suffix": ext_suffix,
    }
    sdk_json = project.Command(
        env,
        name="popcorn-sdk-json",
        target=sdk_json_path,
        source=[project_dir / "popcorn_build" / "plugin_sdk.py", sdk_compiler],
        command=[
            interpreter,
            "-m",
            "popcorn_build.plugin_sdk",
            str(sdk_json_path),
            json.dumps(sdk_fields, sort_keys=True),
        ],
        cwd=project_dir,
        restat=True,
    )
    sdk_files = [
        *(
            (f"{SDK}/include/{relative}", source)
            for relative, source in sdk_headers(
                native_include, reflex_root, prefixes["nanobind"], prefixes["robin-map"]
            )
        ),
        (f"{SDK}/include/popcorn/plugin/build_info.hpp", build_info),
        (f"{SDK}/nanobind/libnanobind-runtime.a", nanobind_runtime()),
    ]

    def sdk_install(prefix: str, no_prefix: bool) -> list[Target]:
        """One InstallAs per SDK file, each target named @p prefix-<path with dashes>."""
        return [
            project.InstallAs(
                destination,
                source,
                name=f"{prefix}-{destination.replace('/', '-')}",
                no_prefix=no_prefix,
            )
            for destination, source in sdk_files
        ]

    staged_sdk = sdk_install("stage-sdk", no_prefix=True)
    for staged in staged_sdk:
        staged.build_tier = "default"
    project.Alias("stage-sdk", sdk_json, *staged_sdk)
    if build_tests:
        xmod_ext = add_extension(
            "popcorn_testing", "_xmod", ["tests/native/support/xmod_module.cpp"]
        )
        xmod_ext.link(core)
        add_stub(xmod_ext, "popcorn_testing", "_xmod", after=[stub])
        xmod_static = add_extension(
            "popcorn_testing.xmod_static", "_xmod", ["tests/native/support/xmod_module.cpp"]
        )
        xmod_static.link(core_static)
        xmod_domain = add_extension(
            "popcorn_testing.xmod_domain", "_xmod", ["tests/native/support/xmod_domain.cpp"]
        )
        xmod_domain.private.compile_flags.append("-DNB_DOMAIN=other")
        xmod_domain.link(core)

    if building_wheel:
        project.Alias(
            "wheel",
            project.Install("popcorn", [ext, stub, *shared_libraries, *obs_python], name="install"),
            project.InstallAs("popcorn/arm/_regs.py", regs, name="install-regs"),
            project.Install("popcorn/devices", [devices_ext, devices_stub], name="install-devices"),
            project.InstallAs(f"{SDK}/sdk.json", sdk_json, name="install-sdk-json"),
            *sdk_install("install-sdk", no_prefix=False),
            *add_package_install("popcorn", python_package, name="install"),
        )

check_core_links(shared_libraries, static_libraries)
check.register()
schema.register()
