"""`--popcorn-part`: every machine the plugin builds uses the replaced part, FR-PYT-1."""

from __future__ import annotations

import json
import textwrap
from pathlib import Path
from typing import cast

import pytest

from .helpers import CONFTEST, M33_JSON

pytest_plugins = ["pytester"]

BASE_PART = "STM32U575QGIx"
VARIANT_PART = "STM32U575CITx"
PART = "STM32U585ZITx"
NO_ASYNCIO = ("-p", "no:asyncio")

DESCRIPTIONS = """
from pathlib import Path

import popcorn
import popcorn.pytest
import pytest
from popcorn.units import us

BASE = popcorn.MachineDescription(part="STM32U575QGIx", images=[])
FULL = BASE.with_configs(variant=BASE.replace(part="STM32U575CITx", strict=True))


@pytest.fixture
def description():
    return FULL


@popcorn.pytest.machine
def plain():
    return FULL


@popcorn.pytest.machine(config="variant")
def configured():
    return FULL


@popcorn.pytest.system
def linked():
    return popcorn.SystemDescription(
        members={"a": FULL, "b": FULL},
        links=[popcorn.UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)],
    )
"""

TESTS = """
import json
import os
from pathlib import Path

from popcorn.pytest._session import SESSION


def test_plain(plain):
    Path(os.environ["PART_RECORD"], "plain.json").write_text(
        json.dumps({"plain": plain.description.part})
    )


def test_configured(configured):
    Path(os.environ["PART_RECORD"], "configured.json").write_text(
        json.dumps({"configured": configured.description.part})
    )


async def test_system_members(linked):
    Path(os.environ["PART_RECORD"], "system.json").write_text(
        json.dumps({n: m.description.part for n, m in linked.members.items()})
    )


def test_factory(machine_factory, description):
    base = machine_factory(description)
    variant = machine_factory(description, config="variant")
    Path(os.environ["PART_RECORD"], "factory.json").write_text(
        json.dumps({"base": base.description.part, "variant": variant.description.part})
    )


def test_keys(request, plain, configured):
    keys = [key.identity for key in request.config.stash[SESSION].keys]
    Path(os.environ["PART_RECORD"], "keys.json").write_text(json.dumps(sorted(keys)))
"""


def write_suite(pytester: pytest.Pytester, tests: str = TESTS) -> Path:
    pytester.makeconftest(CONFTEST + textwrap.dedent(DESCRIPTIONS))
    pytester.makepyfile(test_part=tests)
    record = pytester.mkdir("record")
    return record


def run(
    pytester: pytest.Pytester, record: Path, *args: str, monkeypatch: pytest.MonkeyPatch
) -> tuple[pytest.RunResult, dict[str, object]]:
    monkeypatch.setenv("PART_RECORD", str(record))
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, *args)
    found: dict[str, object] = {}
    for path in sorted(record.glob("*.json")):
        found[path.stem] = json.loads(path.read_text())
        path.unlink()
    return result, found


CASES = {
    "test_plain": ("plain", {"plain": BASE_PART}, {"plain": PART}),
    "test_configured": (
        "configured",
        {"configured": VARIANT_PART},
        {"configured": PART},
    ),
    "test_system_members": (
        "system",
        {"a": BASE_PART, "b": BASE_PART},
        {"a": PART, "b": PART},
    ),
    "test_factory": (
        "factory",
        {"base": BASE_PART, "variant": VARIANT_PART},
        {"base": PART, "variant": PART},
    ),
}


@pytest.mark.parametrize("test", CASES)
def test_the_option_replaces_the_part_of_the_machines_a_test_builds(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, test: str
) -> None:
    record, before, after = CASES[test]
    folder = write_suite(pytester)
    control, kept = run(pytester, folder, "-n", "0", "-k", test, monkeypatch=monkeypatch)
    control.assert_outcomes(passed=1)
    assert kept[record] == before
    result, replaced = run(
        pytester, folder, "-n", "0", "-k", test, f"--popcorn-part={PART}", monkeypatch=monkeypatch
    )
    result.assert_outcomes(passed=1)
    assert replaced[record] == after
    result.stdout.fnmatch_lines(
        [f"popcorn: part {PART} (--popcorn-part)", f"popcorn: 1 of 1 tests ran on {PART}"]
    )


def test_the_boot_key_follows_the_part(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    folder = write_suite(pytester)
    _, kept = run(pytester, folder, "-n", "0", "-k", "test_keys", monkeypatch=monkeypatch)
    _, replaced = run(
        pytester,
        folder,
        "-n",
        "0",
        "-k",
        "test_keys",
        f"--popcorn-part={PART}",
        monkeypatch=monkeypatch,
    )
    before = cast(list[str], kept["keys"])
    after = cast(list[str], replaced["keys"])
    assert len(before) == len(after) == 2
    assert set(before).isdisjoint(after)


def test_a_run_with_overrides_under_xdist_exits_0_and_counts_them(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    pytest.importorskip("xdist")
    record = write_suite(pytester)
    result, found = run(
        pytester, record, "-n", "2", f"--popcorn-part={PART}", monkeypatch=monkeypatch
    )
    assert result.ret == pytest.ExitCode.OK
    assert found["configured"] == {"configured": PART}
    result.stdout.fnmatch_lines([f"popcorn: 5 of 5 tests ran on {PART}"])


@pytest.mark.parametrize("workers", ["0", "2"])
def test_a_test_that_builds_no_machine_exits_1(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, workers: str
) -> None:
    if workers != "0":
        pytest.importorskip("xdist")
    record = write_suite(pytester, "def test_nothing():\n    pass\n")
    result, _ = run(
        pytester, record, "-n", workers, f"--popcorn-part={PART}", monkeypatch=monkeypatch
    )
    result.assert_outcomes(passed=1)
    assert result.ret == pytest.ExitCode.TESTS_FAILED
    result.stdout.fnmatch_lines([f"popcorn: 0 of 1 tests ran on {PART}"])
    clean, _ = run(pytester, record, "-n", workers, monkeypatch=monkeypatch)
    assert clean.ret == pytest.ExitCode.OK


def test_a_description_with_a_file_is_refused_naming_the_fixture(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    (pytester.path / "board.svd").write_text("<device/>")
    pytester.makeconftest(CONFTEST + textwrap.dedent(f"""
        from pathlib import Path

        import popcorn
        import popcorn.pytest
        import pytest

        FILED = popcorn.MachineDescription(
            part="POPCORN-M33-TEST", description=Path({str(M33_JSON)!r}), images=[]
        )
        SVD = popcorn.MachineDescription(
            part="STM32U575QGIx", svd=Path(__file__).parent / "board.svd", images=[]
        )


        @popcorn.pytest.machine
        def filed():
            return FILED


        @popcorn.pytest.machine
        def svd():
            return SVD


        @pytest.fixture
        def descriptions():
            return FILED, SVD
        """))
    pytester.makepyfile("""
        import popcorn
        import pytest

        def test_filed(filed):
            pass

        def test_svd(svd):
            pass

        def test_factory(machine_factory, descriptions):
            for description in descriptions:
                with pytest.raises(popcorn.ConfigError, match="machine_factory.*--popcorn-part"):
                    machine_factory(description)
        """)
    monkeypatch.setenv("PART_RECORD", str(pytester.path))
    result = pytester.runpytest_subprocess(*NO_ASYNCIO, "-n", "0", f"--popcorn-part={PART}")
    result.assert_outcomes(passed=1, errors=2)
    result.stdout.fnmatch_lines(
        [
            "*fixture 'filed': --popcorn-part*description=*",
            "*fixture 'svd': --popcorn-part*svd=*",
        ]
    )


SKIPPING = """
import popcorn
import popcorn.pytest
from pathlib import Path

ABSENT = popcorn.MachineDescription(
    part="STM32U575QGIx", images=[popcorn.Elf(Path(__file__).parent / "absent.elf")],
    missing="skip",
)


@popcorn.pytest.machine
def absent():
    return ABSENT
"""


@pytest.mark.parametrize("workers", ["0", "2"])
def test_skipped_tests_do_not_count_as_run_on_the_part(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, workers: str
) -> None:
    if workers != "0":
        pytest.importorskip("xdist")
    pytester.makeconftest(CONFTEST + SKIPPING)
    pytester.makepyfile("def test_absent(absent):\n    pass\n")
    record = pytester.mkdir("record")
    result, _ = run(
        pytester, record, "-n", workers, f"--popcorn-part={PART}", monkeypatch=monkeypatch
    )
    result.assert_outcomes(skipped=1)
    assert result.ret == pytest.ExitCode.TESTS_FAILED
    result.stdout.fnmatch_lines(
        [
            f"popcorn: 0 of 1 tests ran on {PART}",
            f"popcorn: no test ran on {PART}, exit status 1",
        ]
    )
    clean, _ = run(pytester, record, "-n", workers, monkeypatch=monkeypatch)
    assert clean.ret == pytest.ExitCode.OK


STATUS = """

def pytest_report_teststatus(report):
    if report.skipped:
        return "skipped", "s", "SKIP"
    return None
"""


@pytest.mark.parametrize("workers", ["0", "2"])
def test_a_conftest_status_hook_does_not_hide_the_skipped_tests(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, workers: str
) -> None:
    if workers != "0":
        pytest.importorskip("xdist")
    pytester.makeconftest(CONFTEST + SKIPPING + STATUS)
    pytester.makepyfile("def test_absent(absent):\n    pass\n")
    record = pytester.mkdir("record")
    result, _ = run(
        pytester, record, "-n", workers, f"--popcorn-part={PART}", monkeypatch=monkeypatch
    )
    assert result.ret == pytest.ExitCode.TESTS_FAILED
    result.stdout.fnmatch_lines([f"popcorn: 0 of 1 tests ran on {PART}"])


SUBTESTS = """
import pytest


def test_plain(plain, subtests):
    with subtests.test(msg="skipped part"):
        pytest.skip("not here")
"""


def test_a_skipped_subtest_does_not_uncount_its_test(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    record = write_suite(pytester, SUBTESTS)
    result, _ = run(pytester, record, "-n", "0", f"--popcorn-part={PART}", monkeypatch=monkeypatch)
    assert result.ret == pytest.ExitCode.OK
    result.stdout.fnmatch_lines([f"popcorn: 1 of 1 tests ran on {PART}"])


XFAILING = """
import pytest


@pytest.mark.xfail
def test_plain(plain):
    assert False
"""


def test_an_expected_failure_counts_as_run_on_the_part(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    record = write_suite(pytester, XFAILING)
    result, _ = run(pytester, record, "-n", "0", f"--popcorn-part={PART}", monkeypatch=monkeypatch)
    assert result.ret == pytest.ExitCode.OK
    result.stdout.fnmatch_lines([f"popcorn: 1 of 1 tests ran on {PART}"])


def test_a_run_that_fails_for_its_own_reasons_prints_no_exit_status_reason(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch
) -> None:
    record = write_suite(pytester, "def test_nothing():\n    assert False\n")
    result, _ = run(pytester, record, "-n", "0", f"--popcorn-part={PART}", monkeypatch=monkeypatch)
    assert result.ret == pytest.ExitCode.TESTS_FAILED
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines([f"popcorn: 0 of 1 tests ran on {PART}"])
    assert "exit status 1" not in result.stdout.str()


def test_an_unknown_part_is_refused_once_at_configure(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(tmp_path / "no-db"))
    monkeypatch.delenv("POPCORN_STM32_DESCRIPTION_DIRS", raising=False)
    record = write_suite(pytester)
    result, _ = run(pytester, record, "--popcorn-part=STM32U575QGJx", monkeypatch=monkeypatch)
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines(["*--popcorn-part*STM32U575QGJx*close matches*STM32U575QGIx*"])
    assert result.stdout.str().count("STM32U575QGJx") == 0


def test_a_description_directory_does_not_excuse_an_unknown_part(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    descriptions = tmp_path / "descriptions"
    descriptions.mkdir()
    monkeypatch.setenv("POPCORN_STM32_DESCRIPTION_DIRS", str(descriptions))
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(tmp_path / "no-db"))
    record = write_suite(pytester)
    result, _ = run(pytester, record, "--popcorn-part=STM32U575QGJx", monkeypatch=monkeypatch)
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    assert result.stderr.str().count("STM32U575QGJx") == 1


def test_a_part_of_a_description_directory_is_known(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    descriptions = tmp_path / "descriptions"
    descriptions.mkdir()
    (descriptions / "STM32U5A5ZJTxQ.json").write_text("{}")
    monkeypatch.setenv("POPCORN_STM32_DESCRIPTION_DIRS", str(descriptions))
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(tmp_path / "no-db"))
    record = write_suite(pytester)
    result, _ = run(
        pytester, record, "--collect-only", "--popcorn-part=STM32U5A5ZJTxQ", monkeypatch=monkeypatch
    )
    assert result.ret != pytest.ExitCode.USAGE_ERROR


def test_a_part_of_the_cubemx_db_is_known_with_no_directory_set(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    (tmp_path / "db" / "mcu").mkdir(parents=True)
    (tmp_path / "db" / "mcu" / "STM32U5A5ZJTxQ.xml").write_text("<Mcu/>")
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(tmp_path / "db"))
    monkeypatch.delenv("POPCORN_STM32_DESCRIPTION_DIRS", raising=False)
    record = write_suite(pytester)
    known, _ = run(
        pytester, record, "--collect-only", "--popcorn-part=STM32U5A5ZJTxQ", monkeypatch=monkeypatch
    )
    assert known.ret != pytest.ExitCode.USAGE_ERROR
    unknown, _ = run(
        pytester, record, "--collect-only", "--popcorn-part=STM32U5A5ZJTxW", monkeypatch=monkeypatch
    )
    assert unknown.ret == pytest.ExitCode.USAGE_ERROR
    unknown.stderr.fnmatch_lines(["*close matches*STM32U5A5ZJTxQ*"])


def test_an_ambiguous_cpn_is_refused_at_configure(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    descriptions = tmp_path / "descriptions"
    descriptions.mkdir()
    for stem in ("STM32U5A5Zx", "STM32U5A5xJ"):
        (descriptions / f"{stem}.json").write_text("{}")
    monkeypatch.setenv("POPCORN_STM32_DESCRIPTION_DIRS", str(descriptions))
    record = write_suite(pytester)
    result, _ = run(pytester, record, "--popcorn-part=STM32U5A5ZJ", monkeypatch=monkeypatch)
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    result.stderr.fnmatch_lines(["*--popcorn-part*STM32U5A5Zx, STM32U5A5xJ*"])


def test_a_forced_local_build_skips_the_shipped_parts(
    pytester: pytest.Pytester, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    (tmp_path / "db").mkdir()
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(tmp_path / "db"))
    monkeypatch.delenv("POPCORN_STM32_DESCRIPTION_DIRS", raising=False)
    monkeypatch.setenv("POPCORN_STM32_BUILD", "local")
    record = write_suite(pytester)
    result, _ = run(pytester, record, f"--popcorn-part={PART}", monkeypatch=monkeypatch)
    assert result.ret == pytest.ExitCode.USAGE_ERROR
    assert result.stderr.str().count(PART) == 1
    monkeypatch.setenv("POPCORN_STM32_BUILD", "bogus")
    bad, _ = run(pytester, record, f"--popcorn-part={PART}", monkeypatch=monkeypatch)
    assert bad.ret == pytest.ExitCode.USAGE_ERROR
    bad.stderr.fnmatch_lines(["*--popcorn-part*POPCORN_STM32_BUILD*"])
