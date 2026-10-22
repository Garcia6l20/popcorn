"""The `pytest` and `plugin` extras, and no pytest import outside `popcorn.pytest`."""

from __future__ import annotations

import ast
import tomllib
from importlib.metadata import distribution
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[3]
PACKAGE = ROOT / "python" / "popcorn"
PYPROJECT = ROOT / "pyproject.toml"
FORBIDDEN = ("pytest", "_pytest", "popcorn.pytest")


def module_name(path: Path, root: Path) -> str:
    parts = list(path.relative_to(root.parent).with_suffix("").parts)
    if parts[-1] == "__init__":
        parts.pop()
    return ".".join(parts)


def absolute(node: ast.ImportFrom, module: str, is_package: bool) -> str:
    if node.level == 0:
        return node.module or ""
    base = module.split(".")
    if not is_package:
        base.pop()
    if node.level > 1:
        base = base[: len(base) - node.level + 1]
    return ".".join([*base, node.module] if node.module else base)


def is_forbidden(name: str) -> bool:
    return any(name == root or name.startswith(root + ".") for root in FORBIDDEN)


def imported(source: str, module: str, is_package: bool) -> list[str]:
    names: list[str] = []
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, ast.Import):
            names += [alias.name for alias in node.names]
        elif isinstance(node, ast.ImportFrom):
            target = absolute(node, module, is_package)
            names.append(target)
            names += [f"{target}.{alias.name}" for alias in node.names]
    return names


def pytest_imports(root: Path) -> list[str]:
    found: list[str] = []
    for path in sorted(root.rglob("*.py")):
        if (root / "pytest") in path.parents:
            continue
        module = module_name(path, root)
        for name in imported(path.read_text(), module, path.name == "__init__.py"):
            if is_forbidden(name):
                found.append(f"{module}: {name}")
    return found


def project() -> dict[str, Any]:
    return tomllib.loads(PYPROJECT.read_text())["project"]


def test_the_project_declares_the_pytest_extra() -> None:
    assert project()["optional-dependencies"]["pytest"] == ["pytest>=9.1"]


def test_the_project_declares_the_plugin_extra() -> None:
    assert project()["optional-dependencies"]["plugin"] == ["pcons==0.31.0", "ninja"]


def test_pytest_is_required_by_no_extra_but_pytest() -> None:
    declared = project()
    groups = {"": declared["dependencies"], **declared["optional-dependencies"]}
    assert {
        name: [line for line in lines if line.startswith("pytest")]
        for name, lines in groups.items()
    } == {name: ["pytest>=9.1"] if name == "pytest" else [] for name in groups}


def test_the_pytest_plugin_entry_point_stays_registered() -> None:
    assert project()["entry-points"]["pytest11"] == {"popcorn.pytest": "popcorn.pytest"}


def test_the_build_backend_publishes_the_extras() -> None:
    extras = distribution("popcorn").metadata.get_all("Provides-Extra") or []
    assert {"pytest", "plugin"} <= set(extras)


def test_no_module_outside_popcorn_pytest_imports_pytest() -> None:
    assert pytest_imports(PACKAGE) == []


def test_every_import_form_reaching_pytest_is_found(tmp_path: Path) -> None:
    root = tmp_path / "popcorn"
    (root / "pytest").mkdir(parents=True)
    (root / "sub").mkdir()
    (root / "pytest" / "__init__.py").write_text("import pytest\n")
    (root / "__init__.py").write_text("from . import pytest\n")
    (root / "units.py").write_text("import pytest\n")
    (root / "a.py").write_text("from _pytest.fixtures import FixtureDef\n")
    (root / "b.py").write_text("import popcorn.pytest._cache\n")
    (root / "c.py").write_text("from popcorn import pytest\n")
    (root / "d.py").write_text("from .pytest import _cache\n")
    (root / "sub" / "__init__.py").write_text("from ..pytest._cache import locked\n")
    (root / "sub" / "e.py").write_text("def f() -> None:\n    from .. import pytest\n")
    (root / "fine.py").write_text("import pytest_xdist\nfrom popcorn import plugin\n")
    assert pytest_imports(root) == [
        "popcorn: popcorn.pytest",
        "popcorn.a: _pytest.fixtures",
        "popcorn.a: _pytest.fixtures.FixtureDef",
        "popcorn.b: popcorn.pytest._cache",
        "popcorn.c: popcorn.pytest",
        "popcorn.d: popcorn.pytest",
        "popcorn.d: popcorn.pytest._cache",
        "popcorn.sub: popcorn.pytest._cache",
        "popcorn.sub: popcorn.pytest._cache.locked",
        "popcorn.sub.e: popcorn.pytest",
        "popcorn.units: pytest",
    ]
