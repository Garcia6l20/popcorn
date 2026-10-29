import sys
import sysconfig
from collections.abc import Collection, Sequence
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


def nanobind_runtime() -> Target:
    """The static nanobind runtime, built once and linked into every extension and the SDK."""
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


def _package_dir(package: str) -> str:
    """The directory of the dotted @p package under build/, such as popcorn/devices."""
    return package.replace(".", "/")


def _origin_of_core(package: str) -> str:
    """The rpath from build/@p package, where an extension lands, to build/popcorn."""
    top, *rest = package.split(".")
    up = "/.." * len(rest)
    return f"$$ORIGIN{up}" if top == "popcorn" else f"$$ORIGIN{up}/../popcorn"


def add_extension(package: str, name: str, sources: Sequence[str | Path]) -> Target:
    """A CPython extension module importable as @p package.@p name, built into build/@p package.

    @p package is dotted, a subpackage lands in its directory, such as build/popcorn/devices.
    """
    project = context.current_project
    ext = project.SharedLibrary(f"{package}.{name}", project.default_environment, sources=sources)
    ext.output_name = name
    ext.output_prefix = f"{_package_dir(package)}/"
    ext.output_suffix = ext_suffix
    ext.private.compile_flags.append("-fvisibility=hidden")
    ext.private.link_flags.append(f"-Wl,-rpath,{_origin_of_core(package)}")
    ext.link(nanobind_runtime())
    ext.link(fetched_package("reflex"))
    return ext


def add_stub(
    extension: Target,
    package: str,
    name: str,
    after: Sequence[Target] = (),
    include_private: bool = False,
) -> Target:
    """Generate build/@p package/@p name.pyi with nanobind's stubgen from the built extension.

    @p include_private keeps names with one leading or trailing underscore, so Python code type
    checks against the real signatures of private bindings.
    """
    project = context.current_project
    stub_path = build_dir / _package_dir(package) / f"{name}.pyi"
    stub = project.Command(
        project.default_environment,
        name=f"{package}.{name}-stub",
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
            *(["--include-private"] if include_private else []),
        ],
    )
    stub.depends(extension, *after)
    return stub


def package_files(package_dir: Path) -> list[Path]:
    """Python sources and package data under @p package_dir, recursively, sorted."""
    return sorted(
        path
        for pattern in ("*.py", "*.toml", "*.json.xz", "py.typed")
        for path in package_dir.rglob(pattern)
    )


def prune_stage(stage_dir: Path, package_dir: Path, keep: Collection[str] = ()) -> list[Path]:
    """Removes every file under @p stage_dir that matches a package_files pattern and has no
    source at the same relative path under @p package_dir, except the relative paths in @p keep,
    files the build generates into the stage. Returns the removed paths, sorted.

    Run when the build script runs, before the stage is installed, so a source deleted since the
    last build leaves no staged copy behind.
    """
    sources = {path.relative_to(package_dir).as_posix() for path in package_files(package_dir)}
    removed: list[Path] = []
    for path in package_files(stage_dir):
        relative = path.relative_to(stage_dir).as_posix()
        if relative not in sources and relative not in keep:
            path.unlink()
            removed.append(path)
    return removed


def add_package_install(
    package: str, package_dir: Path, name: str, no_prefix: bool = False
) -> list[Target]:
    """One InstallAs per file of package_files(@p package_dir), each at @p package/<relative path>.

    pcons Install puts every source at its basename in one directory, which would flatten
    subpackages onto each other. Each target is named @p name-<relative path with dashes>.
    """
    project = context.current_project
    installed: list[Target] = []
    for path in package_files(package_dir):
        relative = path.relative_to(package_dir).as_posix()
        installed.append(
            project.InstallAs(
                f"{package}/{relative}",
                path,
                name=f"{name}-{relative.replace('/', '-')}",
                no_prefix=no_prefix,
            )
        )
    return installed
