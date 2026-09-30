"""The on-disk boot snapshot cache, FR-SNAP-3: a second session skips the boot."""

from __future__ import annotations

import shutil

import pytest

from .coverage_suite import write_suite
from .helpers import CALLEE_ELF, DRIVER_ELF, M33_JSON, write_conftest, write_project

pytest_plugins = ["pytester"]

BASE = ("-p", "no:asyncio")

STATE = """
import hashlib
from pathlib import Path

def test_state(machine):
    sram = next(r for r in machine.description.regions if r.name == "SRAM")
    digest = hashlib.sha256(machine.mem.read(sram.start, sram.size)).hexdigest()
    with Path("states.txt").open("a") as states:
        states.write(f"{machine.pc} {machine.instructions} {digest}\\n")
"""


def run(pytester: pytest.Pytester, *args: str) -> pytest.RunResult:
    return pytester.runpytest(*BASE, *args)


def summary(result: pytest.RunResult) -> str:
    return next(
        line for line in result.outlines if line.startswith("popcorn: ") and "key(s)" in line
    )


def test_a_second_session_restores_the_same_state(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(STATE)
    first = run(pytester)
    first.assert_outcomes(passed=1)
    assert summary(first) == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    second = run(pytester)
    second.assert_outcomes(passed=1)
    assert summary(second) == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    states = (pytester.path / "states.txt").read_text().splitlines()
    assert len(states) == 2 and states[0] == states[1]
    boot = pytester.path / ".pytest_cache" / "d" / "popcorn" / "boot"
    assert sorted(path.suffix for path in boot.iterdir()) == [".json", ".lock", ".snap"]


def test_changes_invalidate_the_entry(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    image = pytester.path / "app.elf"
    shutil.copyfile(DRIVER_ELF, image)
    project = pytester.path / "popcorn.toml"

    def write(extra: str = "") -> None:
        project.write_text(
            f'[mcu]\npart = "POPCORN-M33-TEST"\ndescription = "{M33_JSON}"\n'
            f'[image.app]\nelf = "{image}"\n{extra}'
        )

    def boots(*args: str) -> str:
        result = run(pytester, *args)
        result.assert_outcomes(passed=1)
        result.stdout.no_fnmatch_line("*dropped*")
        return summary(result)

    pytester.makepyfile("def test_boot(machine):\n    pass\n")
    write()
    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boots() == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    write("# a comment\n")
    assert boots() == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    write("[boot]\nbudget = 1000\n")
    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boots("--popcorn-strict") == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boots("--popcorn-strict") == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    shutil.copyfile(CALLEE_ELF, image)
    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boots("--cache-clear") == "popcorn: 1 key(s), 1 boot(s), 0 from cache"


def test_model_sources_invalidate_the_entry(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    models = pytester.mkdir("models")
    (models / "mymodels.py").write_text(
        "from popcorn.model import Model\n\nimport mymodels_util\n\n\n"
        "class Plain(Model):\n    NAME = 'first'\n"
    )
    (models / "mymodels_util.py").write_text("VALUE = 1\n")
    write_project(
        pytester,
        extra=f'[board.TESTDEV]\nmodel = "mymodels:Plain"\n[models]\npython = ["{models}"]\n',
    )
    pytester.makepyfile("def test_boot(machine):\n    pass\n")

    def boots() -> str:
        result = pytester.runpytest_subprocess(*BASE)
        result.assert_outcomes(passed=1)
        result.stdout.no_fnmatch_line("*dropped*")
        return summary(result)

    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boots() == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    (models / "mymodels.py").write_text(
        (models / "mymodels.py").read_text().replace("'first'", "'second'")
    )
    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    (models / "mymodels_util.py").write_text("VALUE = 2\n")
    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boots() == "popcorn: 1 key(s), 0 boot(s), 1 from cache"


def test_a_corrupt_entry_is_dropped_and_booted_again(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(STATE)
    run(pytester).assert_outcomes(passed=1)
    boot = pytester.path / ".pytest_cache" / "d" / "popcorn" / "boot"
    (snap,) = boot.glob("*.snap")
    snap.write_bytes(snap.read_bytes()[:100])
    result = run(pytester)
    result.assert_outcomes(passed=1)
    assert summary(result) == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    result.stdout.fnmatch_lines(["popcorn: 1 unreadable boot cache entry dropped and booted again"])
    assert summary(run(pytester)) == "popcorn: 1 key(s), 0 boot(s), 1 from cache"


def test_without_the_cache_plugin_every_session_boots(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile(STATE)
    for _ in range(2):
        result = run(pytester, "-p", "no:cacheprovider")
        assert summary(result) == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert not (pytester.path / ".pytest_cache").exists()


def test_cold_workers_boot_once(pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("def test_boot(machine):\n    pass\n")
    result = pytester.runpytest_subprocess(*BASE, "-n", "4", "--dist", "each", timeout=60)
    result.assert_outcomes(passed=4)
    assert summary(result) == "popcorn: 1 key(s), 1 boot(s), 3 from cache"


def test_cached_boots_give_the_same_lcov(pytester: pytest.Pytester) -> None:
    pytest.importorskip("xdist")
    write_suite(pytester)
    files: list[bytes] = []
    for name, args in (
        ("cold4", ("-n", "4")),
        ("warm0", ("-n", "0")),
        ("warm4", ("-n", "4")),
        ("nocache", ("-n", "0", "-p", "no:cacheprovider")),
    ):
        result = pytester.runpytest_subprocess(*BASE, *args, f"--popcorn-cov={name}.info")
        result.assert_outcomes(passed=30)
        files.append((pytester.path / f"{name}.info").read_bytes())
    assert files[0] == files[1] == files[2] == files[3]


def test_the_async_lock_wait_keeps_the_loop_running(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, extra="[configs.other]\nboot.budget = 1_000_000\n")
    pytester.makepyfile("""
        import asyncio
        import fcntl
        import os

        from popcorn.pytest._cache import Entry, cache_directory, cache_key

        async def test_waits(machine_factory, popcorn_project, request):
            directory = cache_directory(request.config)
            directory.mkdir(parents=True, exist_ok=True)
            project = type(popcorn_project).load(popcorn_project.path, "other")
            entry = Entry.of(directory, cache_key(project, False))
            holder = os.open(entry.lock, os.O_RDWR | os.O_CREAT)
            fcntl.flock(holder, fcntl.LOCK_EX)
            asyncio.get_running_loop().call_later(0.2, os.close, holder)
            ticks = 0
            done = False

            async def ticker():
                nonlocal ticks
                while not done:
                    ticks += 1
                    await asyncio.sleep(0.01)

            task = asyncio.create_task(ticker())
            machine = await machine_factory.booted(config="other")
            done = True
            await task
            assert machine.pc == machine.sym("alu_mark")
            assert ticks >= 5
        """)
    run(pytester).assert_outcomes(passed=1)


def test_a_model_class_outside_the_model_directories_is_keyed(pytester: pytest.Pytester) -> None:
    elsewhere = pytester.mkdir("elsewhere")
    (elsewhere / "othermod.py").write_text(
        "from popcorn.model import Model\n\n\nclass Plain(Model):\n    NAME = 'first'\n"
    )
    write_conftest(pytester, f"import sys\nsys.path.insert(0, {str(elsewhere)!r})\n")
    models = pytester.mkdir("models")
    write_project(
        pytester,
        extra=f'[board.TESTDEV]\nmodel = "othermod:Plain"\n[models]\npython = ["{models}"]\n',
    )
    pytester.makepyfile("def test_boot(machine):\n    pass\n")

    def boots() -> str:
        result = pytester.runpytest_subprocess(*BASE)
        result.assert_outcomes(passed=1)
        result.stdout.no_fnmatch_line("*dropped*")
        return summary(result)

    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boots() == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    (elsewhere / "othermod.py").write_text(
        (elsewhere / "othermod.py").read_text().replace("'first'", "'second'")
    )
    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"


def test_an_entry_without_boot_coverage_is_a_miss_under_cov(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("def test_boot(machine):\n    pass\n")
    run(pytester).assert_outcomes(passed=1)
    result = run(pytester, "-p", "no:xdist", "--popcorn-cov=a.info")
    result.assert_outcomes(passed=1)
    result.stdout.no_fnmatch_line("*dropped*")
    assert summary(result) == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    again = run(pytester, "-p", "no:xdist", "--popcorn-cov=b.info")
    assert summary(again) == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    assert (pytester.path / "a.info").read_bytes() == (pytester.path / "b.info").read_bytes()


def test_a_hit_restores_the_boot_stop(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        from pathlib import Path

        from popcorn.pytest._session import Key, popcorn_session

        def test_stop(machine, request):
            stop = popcorn_session(request.config).booted(Key(None, False)).boot_stop
            with Path("stops.txt").open("a") as stops:
                stops.write(f"{stop!r}\\n")
        """)
    run(pytester).assert_outcomes(passed=1)
    assert summary(run(pytester)) == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    first, second = (pytester.path / "stops.txt").read_text().splitlines()
    assert first == second
    assert "ADDRESS" in first and "alu_mark" in first


MODELS = """
import os
from pathlib import Path

from popcorn.model import Model


class Loaded(Model):
    def __init__(self, data: Path) -> None:
        super().__init__()
        self.data = data

    def restore(self, data: bytes) -> None:
        if os.environ.get("POPCORN_TEST_BREAK_RESTORE"):
            raise RuntimeError("restore broke")
        super().restore(data)
"""


def write_model_project(pytester: pytest.Pytester) -> None:
    models = pytester.mkdir("models")
    (models / "loaded.py").write_text(MODELS)
    (pytester.path / "table.bin").write_bytes(b"\x01\x02")
    write_project(
        pytester,
        extra=(
            '[board.TESTDEV]\nmodel = "loaded:Loaded"\ndata = "table.bin"\n'
            f'[models]\npython = ["{models}"]\n'
        ),
    )
    pytester.makepyfile("def test_boot(machine):\n    pass\n")


def test_a_parameter_file_is_keyed(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_model_project(pytester)

    def boots() -> str:
        result = pytester.runpytest_subprocess(*BASE)
        result.assert_outcomes(passed=1)
        result.stdout.no_fnmatch_line("*dropped*")
        return summary(result)

    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boots() == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    (pytester.path / "table.bin").write_bytes(b"\x03")
    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"


def test_a_model_restore_failure_falls_back_to_a_boot(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    write_conftest(pytester)
    write_model_project(pytester)
    first = pytester.runpytest_subprocess(*BASE)
    assert summary(first) == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    monkeypatch.setenv("POPCORN_TEST_BREAK_RESTORE", "1")
    broken = pytester.runpytest_subprocess(*BASE)
    broken.stdout.fnmatch_lines(["popcorn: 1 unreadable boot cache entry dropped and booted again"])
    assert summary(broken) == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
