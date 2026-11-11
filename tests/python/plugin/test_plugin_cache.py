"""The on-disk boot snapshot cache, FR-SNAP-3: a second session skips the boot."""

from __future__ import annotations

import importlib
import shutil
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

import pytest
from popcorn.pytest._cache import imported_sources

from .coverage_suite import write_suite
from .helpers import (
    CALLEE_ELF,
    DRIVER_ELF,
    M33_JSON,
    ROOT,
    cov_args,
    write_conftest,
    write_project,
    write_project_conftest,
)

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
    image = pytester.path / "app.elf"
    shutil.copyfile(DRIVER_ELF, image)

    def write(extra: str = "", boot: str = "") -> None:
        (pytester.path / "conftest.py").unlink(missing_ok=True)
        write_conftest(pytester)
        write_project_conftest(
            pytester.path,
            f'popcorn.MachineDescription(part="POPCORN-M33-TEST", '
            f"description=Path({str(M33_JSON)!r}), "
            f"images=[popcorn.Elf(Path({str(image)!r}))], boot=popcorn.Boot({boot}))",
            extra,
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
    write(boot="budget=1000")
    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boots("--popcorn-strict") == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boots("--popcorn-strict") == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    shutil.copyfile(CALLEE_ELF, image)
    assert boots() == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    assert boots("--cache-clear") == "popcorn: 1 key(s), 1 boot(s), 0 from cache"


@pytest.mark.parametrize(
    "imported",
    [
        "import mymodels_util\n\n\nclass Plain(Model):\n    NAME = 'first'\n",
        "from mymodels_util import VALUE\n\n\nclass Plain(Model):\n    NAME = 'first'\n",
        "\n\nclass Plain(Model):\n    NAME = 'first'\n\n    def value(self):\n"
        "        from mymodels_util import VALUE\n\n        return VALUE\n",
    ],
    ids=["module", "constant", "lazy"],
)
def test_model_sources_invalidate_the_entry(pytester: pytest.Pytester, imported: str) -> None:
    write_conftest(pytester)
    models = pytester.mkdir("models")
    (models / "mymodels.py").write_text(f"from popcorn.model import Model\n{imported}")
    (models / "mymodels_util.py").write_text("VALUE = 1\n")
    write_project(
        pytester,
        extra='board={"TESTDEV": popcorn.Periph(model=popcorn.Use(Plain))},',
        imports=f"import sys\nsys.path.insert(0, {str(models)!r})\nfrom mymodels import Plain",
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


def test_imported_packages_are_keyed_and_popcorn_is_not(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    for package in ("keyed_pka", "keyed_pkb"):
        (tmp_path / package / "sub").mkdir(parents=True)
        for name in ("__init__.py", "sub/__init__.py", "sub/util.py"):
            (tmp_path / package / name).write_text("VALUE = 1\n")
    (tmp_path / "keyed_model.py").write_text(
        "import keyed_pka.sub.util\nfrom keyed_pkb.sub import util\n"
        "from popcorn.model import Model\n\n\nclass Plain(Model):\n    pass\n"
    )
    monkeypatch.setattr(sys, "path", [str(tmp_path), *sys.path])
    importlib.invalidate_caches()
    try:
        plain = importlib.import_module("keyed_model").Plain
        files = {Path(line.split()[1]) for line in imported_sources(plain)}
    finally:
        for name in [n for n in sys.modules if n.startswith(("keyed_pk", "keyed_model"))]:
            del sys.modules[name]
    assert files == {
        tmp_path / package / name
        for package in ("keyed_pka", "keyed_pkb")
        for name in ("__init__.py", "sub/__init__.py", "sub/util.py")
    }


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
    result = pytester.runpytest_subprocess(*BASE, "-n", "4", "--dist", "each", timeout=12)
    result.assert_outcomes(passed=4)
    assert summary(result) == "popcorn: 1 key(s), 1 boot(s), 3 from cache"


@pytest.mark.timeout(30)
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
        result = pytester.runpytest_subprocess(*BASE, *args, *cov_args(f"{name}.info"))
        result.assert_outcomes(passed=30)
        files.append((pytester.path / f"{name}.info").read_bytes())
    assert files[0] == files[1] == files[2] == files[3]


def test_the_async_lock_wait_keeps_the_loop_running(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester, configs="other=BASE.replace(boot=BASE.boot.replace(budget=1_000_000))")
    pytester.makepyfile("""
        import asyncio
        import fcntl
        import os

        from popcorn.pytest._cache import Entry, cache_directory, cache_key

        async def test_waits(machine_factory, description, request):
            directory = cache_directory(request.config)
            directory.mkdir(parents=True, exist_ok=True)
            project = description.config("other")
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
            machine = await machine_factory.booted(description, config="other")
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
    write_project(
        pytester,
        extra='board={"TESTDEV": popcorn.Periph(model=popcorn.Use(Plain))},',
        imports="from othermod import Plain",
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
    result = run(pytester, "-p", "no:xdist", *cov_args("a.info"))
    result.assert_outcomes(passed=1)
    result.stdout.no_fnmatch_line("*dropped*")
    assert summary(result) == "popcorn: 1 key(s), 1 boot(s), 0 from cache"
    again = run(pytester, "-p", "no:xdist", *cov_args("b.info"))
    assert summary(again) == "popcorn: 1 key(s), 0 boot(s), 1 from cache"
    assert (pytester.path / "a.info").read_bytes() == (pytester.path / "b.info").read_bytes()


def test_a_hit_restores_the_boot_stop(pytester: pytest.Pytester) -> None:
    write_conftest(pytester)
    write_project(pytester)
    pytester.makepyfile("""
        from pathlib import Path

        from popcorn.pytest._session import Key, popcorn_session

        def test_stop(machine, description, request):
            key = Key(description.digest, False)
            stop = popcorn_session(request.config).booted(key).boot_stop
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
    table = pytester.path / "table.bin"
    write_project(
        pytester,
        extra=(
            'board={"TESTDEV": popcorn.Periph(model=popcorn.Use('
            f"Loaded, data=Path({str(table)!r})))}},"
        ),
        imports=f"import sys\nsys.path.insert(0, {str(models)!r})\nfrom loaded import Loaded",
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


REGISTERED = """
from popcorn.model import Model


class First(Model):
    NAME = "first"


class Second(Model):
    NAME = "second"
"""


def registered_boots(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, board: str = ""
) -> Callable[..., str]:
    monkeypatch.setenv("PYTHONDONTWRITEBYTECODE", "1")
    models = pytester.mkdir("models")
    (models / "regmod.py").write_text(REGISTERED)

    def boots(*registrations: str) -> str:
        for directory in (pytester.path, models):
            shutil.rmtree(directory / "__pycache__", ignore_errors=True)
        (pytester.path / "conftest.py").unlink(missing_ok=True)
        lines = "".join(
            f"register({ip!r}, {glob!r})({cls})\n"
            for ip, glob, cls in (r.split() for r in registrations)
        )
        write_conftest(
            pytester,
            f"import sys\nsys.path.insert(0, {str(models)!r})\n"
            f"from popcorn.model import register\nfrom regmod import First, Second\n{lines}",
        )
        write_project(pytester, extra=board, imports="from regmod import First")
        result = pytester.runpytest_subprocess(*BASE)
        result.assert_outcomes(passed=1)
        result.stdout.no_fnmatch_line("*dropped*")
        return summary(result)

    pytester.makepyfile("def test_boot(machine):\n    pass\n")
    return boots


BOOTED = "popcorn: 1 key(s), 1 boot(s), 0 from cache"
CACHED = "popcorn: 1 key(s), 0 boot(s), 1 from cache"


@pytest.mark.timeout(30)
def test_a_registration_serving_the_machine_is_keyed(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    boots = registered_boots(pytester, monkeypatch)
    assert boots() == BOOTED
    assert boots("nosuchip * First") == CACHED
    assert boots("testdev v2 First") == CACHED
    assert boots("testdev v* First") == BOOTED
    assert boots("testdev v* First") == CACHED
    assert boots("testdev v1 Second") == BOOTED
    regmod = pytester.path / "models" / "regmod.py"
    regmod.write_text(regmod.read_text().replace('"second"', '"changed"'))
    assert boots("testdev v1 Second") == BOOTED
    assert boots() == CACHED


def test_a_registration_under_a_board_model_is_not_keyed(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    boots = registered_boots(
        pytester, monkeypatch, board='board={"TESTDEV": popcorn.Periph(model=popcorn.Use(First))},'
    )
    assert boots() == BOOTED
    assert boots("testdev * Second") == CACHED


def test_the_latest_matching_registration_is_keyed(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    boots = registered_boots(pytester, monkeypatch)
    assert boots("testdev v* First", "testdev v1 Second") == BOOTED
    assert boots("testdev v1 Second") == CACHED
    assert boots("testdev v1 Second", "testdev v* First") == BOOTED
    assert boots("testdev v* First", "testdev v1 Second", "testdev v* First") == CACHED


BUILT_IN = """
import popcorn
from popcorn.model import Model, register, unregister
from popcorn.pytest._cache import cache_key


class Dma(Model):
    pass


project = popcorn.MachineDescription(part="STM32U575QGIx", images=[])
keys = [cache_key(project, False)]
unregister("GPDMA", "STM32U5_dma3_*")
keys.append(cache_key(project, False))
register("GPDMA", "STM32U5_dma3_*")(Dma)
keys.append(cache_key(project, False))
unregister("GPDMA", "STM32U5_dma3_*")
keys.append(cache_key(project, False))
print("keys", *(keys.index(key) for key in keys))
"""


def test_a_removed_built_in_registration_is_keyed(tmp_path: Path) -> None:
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    try:
        mcu("STM32U575QGIx")
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")
    script = tmp_path / "built_in.py"
    script.write_text(BUILT_IN)
    result = subprocess.run(
        [sys.executable, str(script)], cwd=ROOT, capture_output=True, text=True, check=True
    )
    assert result.stdout.splitlines()[-1] == "keys 0 1 2 1"


ANSWER = """
from popcorn.model import Model, on_read


class Answer(Model):
    @on_read("RO")
    def ro(self):
        return 0xCAFE
"""

TESTDEV_RO = 0x4001_0000

TEST_HEADER = f"""
import popcorn
from popcorn.model import register, unregister
from answer import Answer

RO = {TESTDEV_RO:#x}


def served(machine):
    try:
        return machine.mem.u32[RO]
    except popcorn.ModelError:
        return None

"""


def registry_session(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, conftest: str, tests: str
) -> pytest.RunResult:
    monkeypatch.setenv("PYTHONDONTWRITEBYTECODE", "1")
    monkeypatch.setenv("POPCORN_CACHE_DIR", str(pytester.path / "cache"))
    models = pytester.mkdir("models")
    (models / "answer.py").write_text(ANSWER)
    write_conftest(
        pytester,
        f"import sys\nsys.path.insert(0, {str(models)!r})\n"
        f"from popcorn.model import register, unregister\nfrom answer import Answer\n{conftest}",
    )
    write_project(pytester, imports="from answer import Answer")
    pytester.makepyfile(f"{TEST_HEADER}{tests}")
    return pytester.runpytest_subprocess(*BASE, "-p", "no:cacheprovider")


@pytest.mark.timeout(60)
def test_a_registration_made_between_tests_applies_to_the_next_machine(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    result = registry_session(
        pytester,
        monkeypatch,
        "",
        """
def test_a_before(machine):
    assert served(machine) is None
    register("testdev")(Answer)


def test_b_registered(machine):
    assert served(machine) == 0xCAFE
    unregister("testdev")


def test_c_unregistered(machine):
    assert served(machine) is None
""",
    )
    result.assert_outcomes(passed=3)
    assert summary(result).startswith("popcorn: 3 key(s)")


@pytest.mark.timeout(60)
def test_an_unregistration_of_a_registration_made_before_the_first_boot_applies(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    result = registry_session(
        pytester,
        monkeypatch,
        'register("testdev")(Answer)\n',
        """
def test_a_registered(machine):
    assert served(machine) == 0xCAFE
    unregister("testdev")


def test_b_unregistered(machine):
    assert served(machine) is None
""",
    )
    result.assert_outcomes(passed=2)
    assert summary(result).startswith("popcorn: 2 key(s)")


@pytest.mark.timeout(60)
def test_a_session_without_a_registration_boots_once_and_keeps_its_key_text(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    result = registry_session(
        pytester,
        monkeypatch,
        "",
        """
def test_a(machine):
    assert served(machine) is None


def test_b(machine):
    assert served(machine) is None
""",
    )
    result.assert_outcomes(passed=2)
    assert summary(result).startswith("popcorn: 1 key(s)")
    assert "registry=" not in "\n".join(result.outlines)


@pytest.mark.timeout(60)
def test_registry_cycles_keep_one_boot_per_registry_state(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    result = registry_session(
        pytester,
        monkeypatch,
        'register("testdev")(Answer)\nunregister("testdev")\n',
        """
import pytest

def test_cycles(machine):
    for _ in range(1000):
        register("testdev")(Answer)
        unregister("testdev")


@pytest.mark.parametrize("answered", [False, True] * 20)
def test_alternating(machine, answered):
    if answered:
        assert served(machine) == 0xCAFE
        unregister("testdev")
    else:
        assert served(machine) is None
        register("testdev")(Answer)
""",
    )
    result.assert_outcomes(passed=41)
    assert int(summary(result).split()[1]) == 2


REGISTRY_DIGEST = """
from popcorn.model import Model, register, unregister
from popcorn.pytest._cache import registry_digest
from popcorn.pytest._session import Key


class Dma(Model):
    pass


digests = [registry_digest()]
unregister("GPDMA", "STM32U5_dma3_*")
digests.append(registry_digest())
register("GPDMA", "STM32U5_dma3_*")(Dma)
digests.append(registry_digest())
assert registry_digest() is not None and registry_digest() == digests[2]
print("digests", *(digests.index(digest) for digest in digests))
print("empty", digests[0] == "")
print("identity", Key("d", False).identity, "|", Key("d", False, registry="r").identity)
"""


def test_the_registry_digest_covers_removed_and_replaced_registrations(tmp_path: Path) -> None:
    script = tmp_path / "registry_digest.py"
    script.write_text(REGISTRY_DIGEST)
    result = subprocess.run(
        [sys.executable, str(script)], cwd=ROOT, capture_output=True, text=True, check=True
    )
    lines = result.stdout.splitlines()
    assert lines[-3] == "digests 0 1 2"
    assert lines[-2] == "empty True"
    assert lines[-1] == "identity digest=d strict=False | digest=d strict=False registry=r"
