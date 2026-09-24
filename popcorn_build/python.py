import sys
import sysconfig
from collections.abc import Sequence
from pathlib import Path

from pcons import Target, context

from popcorn_build.config import build_dir
from popcorn_build.requirements import package as fetched_package
from popcorn_build.requirements import prefixes

interpreter = sys.executable
ext_suffix: str = sysconfig.get_config_var("EXT_SUFFIX")
python_include = Path(sysconfig.get_paths()["include"])

if not (python_include / "Python.h").is_file():
    raise SystemExit(
        f"popcorn needs the CPython development headers for {interpreter},"
        f" Python.h not found in {python_include}"
    )

_nanobind_runtime: Target | None = None


def _nanobind() -> Target:
    global _nanobind_runtime
    if _nanobind_runtime is None:
        project = context.current_project
        nanobind = prefixes["nanobind"]
        _nanobind_runtime = project.StaticLibrary(
            "nanobind-runtime",
            project.default_environment,
            sources=[nanobind / "src" / "nb_combined.cpp"],
        )
        _nanobind_runtime.public.system_include_dirs.extend(
            [
                nanobind / "include",
                prefixes["robin-map"] / "include",
                prefixes["reflex"] / "py" / "include",
                python_include,
            ]
        )
        _nanobind_runtime.private.compile_flags.extend(
            ["-fno-strict-aliasing", "-fvisibility=hidden", "-Wno-cast-qual"]
        )
    return _nanobind_runtime


def add_extension(package: str, name: str, sources: Sequence[str | Path]) -> Target:
    """A CPython extension module importable as @p package.@p name, built into build/@p package."""
    project = context.current_project
    ext = project.SharedLibrary(f"{package}.{name}", project.default_environment, sources=sources)
    ext.output_name = name
    ext.output_prefix = f"{package}/"
    ext.output_suffix = ext_suffix
    ext.private.compile_flags.append("-fvisibility=hidden")
    ext.link(_nanobind())
    ext.link(fetched_package("reflex"))
    return ext


def add_stub(extension: Target, package: str, name: str, after: Sequence[Target] = ()) -> Target:
    """Generate build/@p package/@p name.pyi with nanobind's stubgen from the built extension."""
    project = context.current_project
    stub_path = build_dir / package / f"{name}.pyi"
    stub = project.Command(
        f"{package}.{name}-stub",
        project.default_environment,
        target=stub_path,
        command=[
            interpreter,
            str(prefixes["nanobind"] / "src" / "stubgen.py"),
            "-q",
            "-m",
            f"{package}.{name}",
            "-o",
            str(stub_path),
            "-i",
            str(build_dir),
        ],
    )
    stub.depends(extension, *after)
    return stub
