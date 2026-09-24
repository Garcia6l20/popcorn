import logging
from pathlib import Path
from typing import Any

from pcons import ImportedTarget, PackageDescription
from pcons.packages.fetch.cli import fetch_package, load_deps_file

from popcorn_build.config import build_dir, project_dir

deps_file = project_dir / "deps.toml"
deps_dir = build_dir / "deps"
pkg_dir = build_dir / "pkg"


def _fetch_missing() -> dict[str, Path]:
    manifest: dict[str, dict[str, Any]] = load_deps_file(deps_file)["packages"]
    missing = [name for name in manifest if not (pkg_dir / f"{name}.pcons-pkg.toml").exists()]
    if missing:
        logging.basicConfig(level=logging.INFO, format="pcons-fetch: %(message)s")
        for name in missing:
            if not fetch_package(name, manifest[name], deps_dir, pkg_dir):
                raise SystemExit(f"popcorn: fetching {name} from deps.toml failed")
    return {
        name: Path(PackageDescription.from_toml(pkg_dir / f"{name}.pcons-pkg.toml").prefix)
        for name in manifest
    }


prefixes = _fetch_missing()


def _describe(name: str) -> PackageDescription:
    prefix = prefixes[name]
    match name:
        case "reflex":
            return PackageDescription(
                name=name, system_include_dirs=[str(prefix / "core" / "include")]
            )
        case "doctest":
            return PackageDescription(name=name, system_include_dirs=[str(prefix)])
        case "unicorn":
            return PackageDescription(
                name=name,
                system_include_dirs=[str(prefix / "include")],
                library_dirs=[str(prefix / "lib")],
                libraries=["unicorn", "m", "pthread"],
                link_flags=["-Wl,--exclude-libs,ALL"],
            )
        case _:
            raise KeyError(f"popcorn: no imported target for fetched package {name!r}")


_imported: dict[str, ImportedTarget] = {}


def package(name: str) -> ImportedTarget:
    """The fetched package @p name as a target of the current project."""
    if name not in _imported:
        description = _describe(name)
        description.prefix = str(prefixes[name])
        description.found_by = "pcons-fetch"
        _imported[name] = ImportedTarget.from_package(description)
    return _imported[name]
