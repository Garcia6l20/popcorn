"""A plugin a `conftest.py` declares builds when its directory is collected, once under xdist."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

from .support import copied

TESTS = "def test_one() -> None:\n    pass\n\n\ndef test_two() -> None:\n    pass\n"


def pytest_run(
    root: Path, cache: Path, *paths: str, extra: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    """`pytest -n 6` over @p paths of the tree at @p root, in a fresh interpreter."""
    environment = {key: value for key, value in os.environ.items() if not key.startswith("PYTEST_")}
    environment["POPCORN_PLUGIN_CACHE"] = str(cache)
    environment.update(extra or {})
    return subprocess.run(
        [sys.executable, "-m", "pytest", "-n", "6", "-p", "no:cacheprovider", "-rA", *paths],
        cwd=root,
        capture_output=True,
        text=True,
        env=environment,
        check=False,
        timeout=600,
    )


def broken_tree(tmp_path: Path) -> Path:
    """A tree whose `a/conftest.py` declares a plugin with a compile error, `b` has none."""
    source = copied("echo", tmp_path)
    code = (source / "echo.cpp").read_text()
    (source / "echo.cpp").write_text(code.replace("return out + 1;", "return out + ;"))
    root = tmp_path / "tree"
    for directory in ("a", "a/deep", "b"):
        (root / directory).mkdir(parents=True)
        (root / directory / f"test_{directory.replace('/', '_')}.py").write_text(TESTS)
    (root / "a" / "conftest.py").write_text(
        f"from popcorn.plugin import Plugin\n\nPLUGIN = Plugin.dev({str(source)!r})\n"
    )
    return root


def test_a_nested_conftests_build_failure_is_one_collection_error(tmp_path: Path) -> None:
    root = broken_tree(tmp_path)
    ran = pytest_run(root, tmp_path / "cache", ".")
    assert ran.returncode != 0
    assert "1 error" in ran.stdout, ran.stdout
    assert "ERROR a - plugin echo: the build failed" in ran.stdout, ran.stdout
    assert "PASSED b/test_b.py::test_one" in ran.stdout
    assert "PASSED b/test_b.py::test_two" in ran.stdout
    assert "test_a.py::" not in ran.stdout
    assert "test_a_deep.py::" not in ran.stdout
    assert "2 passed" in ran.stdout


def test_an_initial_paths_build_failure_is_one_collection_error(tmp_path: Path) -> None:
    root = broken_tree(tmp_path)
    ran = pytest_run(root, tmp_path / "cache", "a")
    assert ran.returncode != 0
    assert "1 error" in ran.stdout, ran.stdout
    assert "ERROR a - plugin echo: the build failed" in ran.stdout, ran.stdout
    assert "passed" not in ran.stdout


def test_six_workers_build_once_and_each_loads_the_plugin(tmp_path: Path) -> None:
    source = copied("echo", tmp_path)
    log = tmp_path / "builds.log"
    loads = tmp_path / "loads"
    loads.mkdir()
    root = tmp_path / "tree"
    (root / "plugin").mkdir(parents=True)
    (root / "plugin" / "conftest.py").write_text(
        f"from popcorn.plugin import Plugin\n\nPLUGIN = Plugin.dev({str(source)!r})\n"
    )
    (root / "plugin" / "test_load.py").write_text(
        "import os\n"
        "from pathlib import Path\n\n"
        "from popcorn.plugin import Plugin\n\n"
        f"MODULE = Plugin.dev({str(source)!r}).module()\n"
        f"Path({str(loads)!r}, os.environ['PYTEST_XDIST_WORKER']).write_text(MODULE.__file__)\n\n"
        + "".join(
            f"def test_{index}() -> None:\n    assert MODULE.EchoBase\n\n" for index in range(12)
        )
    )
    ran = pytest_run(root, tmp_path / "cache", ".", extra={"ECHO_BUILD_LOG": str(log)})
    assert ran.returncode == 0, ran.stdout + ran.stderr
    assert "12 passed" in ran.stdout
    assert len(log.read_text().splitlines()) == 1
    loaded = {path.name: path.read_text() for path in loads.iterdir()}
    assert sorted(loaded) == [f"gw{index}" for index in range(6)]
    assert len(set(loaded.values())) == 1
