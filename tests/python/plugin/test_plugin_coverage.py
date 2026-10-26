"""`--popcorn-cov` without xdist: one lcov file, boot counted once, UC-8."""

from __future__ import annotations

import re
import shutil
from pathlib import Path

import pytest

from .coverage_suite import write_suite
from .helpers import DRIVER_ELF, ROOT, cov_args

pytest_plugins = ["pytester"]

DRIVER_C = ROOT / "tests" / "firmware" / "api" / "driver.c"


def line_of(source: Path, text: str) -> int:
    return next(n for n, line in enumerate(source.read_text().splitlines(), 1) if text in line)


def hits(lcov: str, source: str, line: int) -> int:
    records = lcov.split("end_of_record")
    record = next(r for r in records if re.search(rf"^SF:.*{re.escape(source)}$", r, re.M))
    match = re.search(rf"^DA:{line},(\d+)", record, re.M)
    assert match is not None, f"no DA for {source}:{line}"
    return int(match.group(1))


def test_coverage_without_xdist(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    lcov = pytester.path / "out" / "a.info"
    lcov.parent.mkdir()
    result = pytester.runpytest("-p", "no:asyncio", "-p", "no:xdist", *cov_args("out/a.info"))
    result.assert_outcomes(passed=30)
    result.stdout.fnmatch_lines([f"popcorn: firmware coverage written to {lcov}"])
    text = lcov.read_text()
    assert re.search(r"^SF:.*driver\.c$", text, re.M)
    assert re.search(r"^SF:.*callee\.c$", text, re.M)
    assert hits(text, "driver.c", line_of(DRIVER_C, "alu_rounds = alu_rounds + 1u;")) == 6
    assert hits(text, "driver.c", line_of(DRIVER_C, "if(round == 1000u)")) == 1001
    assert re.search(r"^BRDA:", text, re.M)


def test_no_machine_no_lcov(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    pytester.makepyfile(test_suite="def test_plain():\n    pass\n")
    result = pytester.runpytest("-p", "no:asyncio", "-p", "no:xdist", *cov_args("a.info"))
    result.assert_outcomes(passed=1)
    result.stdout.fnmatch_lines(["popcorn: no firmware coverage collected, no report written"])
    assert not (pytester.path / "a.info").exists()


def test_the_session_directory_holds_one_file_per_process_and_key(
    pytester: pytest.Pytester,
) -> None:
    write_suite(
        pytester,
        """
        from pathlib import Path

        from popcorn.pytest import _coverage

        merge = _coverage.merge_session

        def listed(directory, lcov, **options):
            names = sorted(path.name for path in Path(directory).iterdir())
            Path("files.txt").write_text("\\n".join(names) + "\\n" + str(directory))
            return merge(directory, lcov, **options)

        def pytest_configure(config):
            _coverage.merge_session = listed

        def pytest_unconfigure(config):
            _coverage.merge_session = merge
        """,
    )
    pytester.runpytest("-p", "no:asyncio", "-p", "no:xdist", *cov_args("a.info"))
    *names, directory = (pytester.path / "files.txt").read_text().splitlines()
    assert names[-1] == "main.pcov"
    assert [name.startswith("boot-") and name.endswith(".pcov") for name in names[:-1]] == [
        True,
        True,
    ]
    assert not Path(directory).exists()


def test_spoiled_and_factory_machines_are_counted(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    pytester.makepyfile(test_suite=f"""
        import popcorn

        async def test_spoils(machine):
            await machine.call("negate", 1)
            machine.load(popcorn.Image.elf({str(DRIVER_ELF)!r}))

        async def test_after(machine):
            await machine.call("negate", 2)

        async def test_factory(machine_factory, description):
            other = await machine_factory.booted(description)
            await other.call("negate", 3)
        """)
    result = pytester.runpytest("-p", "no:asyncio", "-p", "no:xdist", *cov_args("a.info"))
    result.assert_outcomes(passed=3)
    text = (pytester.path / "a.info").read_text()
    assert re.search(r"^FNDA:3,negate$", text, re.M)


def test_a_boot_whose_coverage_cannot_be_saved_still_boots(pytester: pytest.Pytester) -> None:
    write_suite(
        pytester,
        """
        from pathlib import Path

        import popcorn.coverage

        SAVE = popcorn.coverage.Coverage.save

        def pytest_configure(config):
            def refuse(self, path):
                if Path(path).name.startswith("boot-"):
                    raise PermissionError(f"refused {path}")
                SAVE(self, path)

            popcorn.coverage.Coverage.save = refuse

        def pytest_unconfigure(config):
            popcorn.coverage.Coverage.save = SAVE
        """,
    )
    result = pytester.runpytest("-p", "no:asyncio", "-p", "no:xdist", *cov_args("a.info"))
    result.assert_outcomes(passed=30)
    result.stdout.fnmatch_lines(
        [
            "popcorn: firmware coverage written to *a.info",
            "popcorn: boot coverage of config=- digest=* strict=False not saved: refused "
            "*boot-*.pcov",
            "popcorn: boot coverage of config=booted digest=* strict=False not saved: refused *",
        ]
    )


def test_an_interrupted_session_keeps_its_pcov_files(pytester: pytest.Pytester) -> None:
    write_suite(pytester)
    pytester.makepyfile(test_suite="""
        async def test_first(machine):
            await machine.call("add", 1, 2)

        def test_interrupts():
            raise KeyboardInterrupt
        """)
    result = pytester.runpytest_subprocess(
        "-p", "no:asyncio", "-p", "no:xdist", *cov_args("a.info")
    )
    assert result.ret == pytest.ExitCode.INTERRUPTED
    result.stdout.fnmatch_lines(
        ["popcorn: session interrupted, no report written, the .pcov files stay in *popcorn-cov-*"]
    )
    assert not (pytester.path / "a.info").exists()
    line = next(line for line in result.outlines if "stay in" in line)
    directory = Path(line.rsplit(" ", 1)[1])
    assert sorted(path.suffix for path in directory.iterdir()) == [".pcov", ".pcov"]
    shutil.rmtree(directory)
