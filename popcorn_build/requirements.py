import functools
import hashlib
import json
import logging
import subprocess
import sys
from pathlib import Path
from typing import Any

import tomli_w
from pcons import ImportedTarget, PackageDescription
from pcons.packages.fetch.cli import (
    build_cmake,
    fetch_package,
    generate_package_description,
    load_deps_file,
)

from popcorn_build.config import build_dir, project_dir
from popcorn_build.firmware import toolchain_found
from popcorn_build.manifest import is_firmware_only, packages_to_fetch
from popcorn_build.options import build_examples, build_tests

deps_file = project_dir / "deps.toml"
deps_dir = build_dir / "deps"
pkg_dir = build_dir / "pkg"

unicorn_cmake_options = {
    "UNICORN_ARCH": "arm",
    "BUILD_SHARED_LIBS": "OFF",
    "UNICORN_LEGACY_STATIC_ARCHIVE": "ON",
    "UNICORN_BUILD_TESTS": "OFF",
    "UNICORN_INSTALL": "ON",
    "CMAKE_POSITION_INDEPENDENT_CODE": "ON",
    "CMAKE_C_VISIBILITY_PRESET": "hidden",
    "CMAKE_C_FLAGS": "-fvisibility=hidden",
}


@functools.cache
def _log_fetches() -> None:
    logging.basicConfig(level=logging.INFO, format="pcons-fetch: %(message)s")


manifest: dict[str, dict[str, Any]] = load_deps_file(deps_file)["packages"]


def _fetch_missing() -> dict[str, Path]:
    wanted = packages_to_fetch(
        manifest, with_firmware=toolchain_found and (build_tests or build_examples)
    )
    missing = [name for name in wanted if not (pkg_dir / f"{name}.pcons-pkg.toml").exists()]
    if missing:
        _log_fetches()
        for name in missing:
            if not fetch_package(name, manifest[name], deps_dir, pkg_dir):
                raise SystemExit(f"popcorn: fetching {name} from deps.toml failed")
    return {
        name: Path(PackageDescription.from_toml(pkg_dir / f"{name}.pcons-pkg.toml").prefix)
        for name in wanted
    }


def _submodule(name: str, marker: str) -> Path:
    prefix = project_dir / "third_party" / name
    if not (prefix / marker).exists():
        raise SystemExit(f"popcorn: {name} submodule missing, run: git submodule update --init")
    return prefix


def _git(source: Path, *args: str) -> str:
    try:
        return subprocess.run(
            ["git", "-C", str(source), *args], capture_output=True, text=True, check=True
        ).stdout.strip()
    except FileNotFoundError, subprocess.CalledProcessError:
        raise SystemExit(
            f"popcorn: cannot read the {source.name} submodule commit, git is needed and"
            f" third_party/{source.name} must be a git checkout (git submodule update --init)"
        ) from None


def _build_cmake_package(
    name: str, source: Path, version: str, options: dict[str, str]
) -> PackageDescription:
    _log_fetches()
    install = deps_dir / "install"
    if not build_cmake(source, deps_dir / "build" / name, install, options):
        raise SystemExit(f"popcorn: building {name} from {source} failed")
    description, pc_files = generate_package_description(name, version, install, "cmake")
    pkg_file = pkg_dir / f"{name}.pcons-pkg.toml"
    pkg_dir.mkdir(parents=True, exist_ok=True)
    description.to_toml(pkg_file)
    if pc_files:
        pc_dirs = sorted({str(path.parent) for path in pc_files})
        with pkg_file.open("ab") as out:
            out.write(b"\n" + tomli_w.dumps({"pkgconfig": {"pc_dirs": pc_dirs}}).encode())
    return description


def _submodule_cmake_package(name: str, options: dict[str, str], library: str) -> tuple[Path, str]:
    source = _submodule(name, "CMakeLists.txt")
    commit = _git(source, "rev-parse", "HEAD")
    dirty = _git(source, "status", "--porcelain") != ""
    digest = hashlib.sha256(json.dumps(options, sort_keys=True).encode()).hexdigest()[:12]
    key = f"{commit}+{digest}"
    label = f"{commit[:12]}-dirty" if dirty else commit[:12]
    pkg_file = pkg_dir / f"{name}.pcons-pkg.toml"
    if not dirty and pkg_file.exists():
        built = PackageDescription.from_toml(pkg_file)
        if built.version == key and (Path(built.prefix) / "lib" / library).exists():
            return Path(built.prefix), label
    if dirty:
        print(f"popcorn: third_party/{name} has local changes, rebuilding {name}", file=sys.stderr)
    return Path(_build_cmake_package(name, source, key, options).prefix), label


prefixes = _fetch_missing()
prefixes["reflex"] = prefixes["reflex-serde"] = _submodule("reflex", "core/include")
prefixes["unicorn"], unicorn_commit = _submodule_cmake_package(
    "unicorn", unicorn_cmake_options, "libunicorn.a"
)
prefixes["unicorn-headers"] = prefixes["unicorn"]


def _describe(name: str) -> PackageDescription:
    prefix = prefixes[name]
    match name:
        case "reflex":
            return PackageDescription(
                name=name, system_include_dirs=[str(prefix / "core" / "include")]
            )
        case "reflex-serde":
            return PackageDescription(
                name=name,
                system_include_dirs=[
                    str(prefix / component / "include") for component in ("core", "poly", "serde")
                ],
            )
        case "doctest":
            return PackageDescription(name=name, system_include_dirs=[str(prefix)])
        case "unicorn":
            return PackageDescription(
                name=name,
                system_include_dirs=[str(prefix / "include")],
                library_dirs=[str(prefix / "lib")],
                libraries=["unicorn", "m", "pthread"],
            )
        case "unicorn-headers":
            return PackageDescription(name=name, system_include_dirs=[str(prefix / "include")])
        case _:
            raise KeyError(f"popcorn: no imported target for fetched package {name!r}")


_imported: dict[str, ImportedTarget] = {}


def package(name: str) -> ImportedTarget:
    """The fetched package @p name as a target of the current project."""
    if name not in _imported:
        description = _describe(name)
        description.prefix = str(prefixes[name])
        description.found_by = (
            "git-submodule" if name.startswith(("reflex", "unicorn")) else "pcons-fetch"
        )
        _imported[name] = ImportedTarget.from_package(description)
    return _imported[name]


def firmware_source(name: str) -> Path:
    """Source tree of the firmware-only package @p name, fetched only with arm-none-eabi-gcc."""
    if name not in manifest or not is_firmware_only(manifest[name]):
        raise SystemExit(f"popcorn: {name} is not a firmware-only package of deps.toml")
    if name not in prefixes:
        raise SystemExit(f"popcorn: firmware package {name} needs arm-none-eabi-gcc on PATH")
    return prefixes[name]
