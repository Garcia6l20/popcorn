import dataclasses
import errno
import json
import sys
from pathlib import Path

import pytest
import reports
from click.testing import CliRunner
from popcorn.cli import cli, main
from popcorn.profiling import FunctionRow, Report


def run(*args: str | Path):
    return CliRunner().invoke(cli, ["profile", "summary", *map(str, args)])


def with_functions(report: Report, selfs: dict[str, int]) -> Report:
    assert report.hotspots is not None
    rows = tuple(FunctionRow(name, 0, own, own, 1) for name, own in selfs.items())
    total = sum(selfs.values())
    return dataclasses.replace(
        report,
        executed=total,
        elapsed_ticks=total,
        elapsed_ps=total * 1000,
        sleep=None,
        irq=None,
        hotspots=dataclasses.replace(report.hotspots, functions=rows),
    )


def three(tmp_path: Path) -> tuple[Report, Report, Report]:
    full = reports.full_report()
    bare = dataclasses.replace(reports.full_report(extra=5), sleep=None, irq=None, hotspots=None)
    small = reports.synthetic(main_self=1)
    reports.write(tmp_path / "b_full.json", full)
    reports.write(tmp_path / "a_bare.json", bare)
    reports.write(tmp_path / "c_small.json", small)
    return full, bare, small


def test_rows_hold_the_numbers_of_the_reports_and_dash_for_absent_sections(
    tmp_path: Path,
) -> None:
    full, bare, small = three(tmp_path)
    assert full.sleep and full.irq and full.hotspots
    result = run(tmp_path)
    assert result.exit_code == 0
    irq = sum(row.self_time_ps for row in full.irq.exceptions) / full.elapsed_ps
    lines = result.stdout.splitlines()
    assert lines[0].split() == ["name", "executed", "ms", "sleep", "irq", "top"]
    assert lines[1].split() == [
        "a_bare",
        f"{bare.executed:,}",
        f"{bare.elapsed_ps / 1e9:.3f}",
        "-",
        "-",
        "-",
    ]
    assert lines[2].split() == [
        "b_full",
        f"{full.executed:,}",
        f"{full.elapsed_ps / 1e9:.3f}",
        f"{100 * full.sleep.share:.1f}%",
        f"{100 * irq:.1f}%",
        full.hotspots.functions[0].name,
    ]
    assert lines[3].split()[:5] == [
        "c_small",
        f"{small.executed:,}",
        f"{small.elapsed_ps / 1e9:.3f}",
        "-",
        "-",
    ]
    total = full.executed + bare.executed + small.executed
    assert lines[-1] == f"3 reports, {total:,} instructions"


def test_columns_are_sized_from_the_data(tmp_path: Path) -> None:
    reports.write(tmp_path / "a.json", reports.synthetic(main_self=10))
    reports.write(tmp_path / "long_name.json", reports.synthetic(main_self=1000))
    assert run(tmp_path).stdout.splitlines()[:3] == [
        "name       executed     ms  sleep  irq  top",
        "a                17  0.000      -    -  main",
        "long_name     1,007  0.001      -    -  main",
    ]


def test_sort_keys_order_rows_descending_with_dash_last_and_name_ascending(
    tmp_path: Path,
) -> None:
    full, bare, small = three(tmp_path)

    def names(*args: str) -> list[str]:
        out = run(*args, tmp_path).stdout.splitlines()[1:4]
        return [line.split()[0] for line in out]

    assert names() == ["a_bare", "b_full", "c_small"]
    assert names("--sort", "name") == ["a_bare", "b_full", "c_small"]
    by_executed = sorted(
        [("a_bare", bare.executed), ("b_full", full.executed), ("c_small", small.executed)],
        key=lambda item: -item[1],
    )
    assert names("--sort", "executed") == [name for name, _ in by_executed]
    assert names("--sort", "ms") == [name for name, _ in by_executed]
    assert names("--sort", "sleep") == ["b_full", "a_bare", "c_small"]
    assert names("--sort", "irq") == ["b_full", "a_bare", "c_small"]


def test_ties_sort_by_name(tmp_path: Path) -> None:
    for name in ("z", "m", "a"):
        reports.write(tmp_path / f"{name}.json", reports.synthetic())
    lines = run("--sort", "executed", tmp_path).stdout.splitlines()
    assert [line.split()[0] for line in lines[1:4]] == ["a", "m", "z"]


def test_json_holds_the_raw_numbers(tmp_path: Path) -> None:
    full, bare, small = three(tmp_path)
    assert full.sleep and full.irq and full.hotspots
    result = run("--json", "--sort", "executed", tmp_path)
    assert result.exit_code == 0
    irq = sum(row.self_time_ps for row in full.irq.exceptions) / full.elapsed_ps
    expected = {
        "reports": [
            {
                "name": "a_bare",
                "executed": bare.executed,
                "elapsed_ps": bare.elapsed_ps,
                "sleep_share": None,
                "irq_share": None,
                "top": None,
            },
            {
                "name": "b_full",
                "executed": full.executed,
                "elapsed_ps": full.elapsed_ps,
                "sleep_share": full.sleep.share,
                "irq_share": irq,
                "top": full.hotspots.functions[0].name,
            },
            {
                "name": "c_small",
                "executed": small.executed,
                "elapsed_ps": small.elapsed_ps,
                "sleep_share": None,
                "irq_share": None,
                "top": "main",
            },
        ],
        "functions": [],
    }
    assert json.loads(result.stdout) == expected
    assert result.stdout == json.dumps(expected, indent=2, sort_keys=True) + "\n"


def make_functions(tmp_path: Path) -> int:
    base = reports.synthetic()
    reports.write(tmp_path / "a.json", with_functions(base, {"f": 30, "g": 10, "k": 10}))
    reports.write(tmp_path / "b.json", with_functions(base, {"f": 5, "g": 10, "h": 14, "m": 1}))
    reports.write(tmp_path / "c.json", with_functions(base, {"k": 4, "f": 1}))
    return 30 + 10 + 10 + 5 + 10 + 14 + 1 + 4 + 1


def test_functions_sum_by_name_and_break_ties_by_name(tmp_path: Path) -> None:
    total = make_functions(tmp_path)
    result = run("--functions", "4", tmp_path)
    assert result.exit_code == 0
    lines = result.stdout.splitlines()
    start = lines.index("") + 1
    assert lines[start].split() == ["function", "self", "share", "reports"]
    rows = [line.split() for line in lines[start + 1 : start + 5]]
    assert rows == [
        ["f", "36", f"{100 * 36 / total:.1f}%", "3"],
        ["g", "20", f"{100 * 20 / total:.1f}%", "2"],
        ["h", "14", f"{100 * 14 / total:.1f}%", "1"],
        ["k", "14", f"{100 * 14 / total:.1f}%", "2"],
    ]
    assert lines[-1] == f"3 reports, {total:,} instructions"


def test_functions_in_json(tmp_path: Path) -> None:
    total = make_functions(tmp_path)
    result = run("--json", "--functions", "2", tmp_path)
    assert json.loads(result.stdout)["functions"] == [
        {"function": "f", "self": 36, "share": 36 / total, "reports": 3},
        {"function": "g", "self": 20, "share": 20 / total, "reports": 2},
    ]


def test_functions_off_prints_no_second_table(tmp_path: Path) -> None:
    make_functions(tmp_path)
    assert "" not in run(tmp_path).stdout.splitlines()


def test_a_file_as_directory_exits_2(tmp_path: Path) -> None:
    file = reports.write(tmp_path / "a.json", reports.synthetic())
    result = run(file)
    assert result.exit_code == 2
    assert "Invalid value" in result.stderr


def test_empty_directory_exits_1(tmp_path: Path) -> None:
    result = run(tmp_path)
    assert result.exit_code == 1
    assert "no *.json report" in result.stderr


def test_bad_file_is_skipped_with_exit_1_and_the_rest_printed(tmp_path: Path) -> None:
    three(tmp_path)
    bad = tmp_path / "bad.json"
    bad.write_text("not json", encoding="utf-8")
    (tmp_path / "later.json.part").write_text("{", encoding="utf-8")
    result = run(tmp_path)
    assert result.exit_code == 1
    assert f"Error: {bad}" in result.stderr
    assert "later" not in result.stdout
    assert result.stdout.splitlines()[-1].startswith("3 reports,")
    assert "bad" not in result.stdout


def test_unknown_sort_key_exits_2(tmp_path: Path) -> None:
    three(tmp_path)
    assert run("--sort", "top", tmp_path).exit_code == 2


class BrokenStdout:
    def write(self, text: str) -> int:
        raise BrokenPipeError(errno.EPIPE, "broken pipe")

    def flush(self) -> None:
        return None


@pytest.mark.parametrize("json_flag", [[], ["--json"]])
def test_broken_pipe_on_stdout_returns_1_quietly(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
    monkeypatch: pytest.MonkeyPatch,
    json_flag: list[str],
) -> None:
    three(tmp_path)
    monkeypatch.setattr(sys, "stdout", BrokenStdout())
    assert main(["profile", "summary", *json_flag, str(tmp_path)]) == 1
    assert capsys.readouterr().err == ""


def shaped(executed: int, elapsed_ps: int, sleep_share: float, irq_ps: int) -> Report:
    full = reports.full_report()
    assert full.sleep and full.irq
    exception = dataclasses.replace(full.irq.exceptions[0], self_time_ps=irq_ps)
    return dataclasses.replace(
        full,
        executed=executed,
        elapsed_ps=elapsed_ps,
        sleep=dataclasses.replace(full.sleep, share=sleep_share),
        irq=dataclasses.replace(full.irq, exceptions=(exception,)),
    )


def test_every_numeric_sort_key_reads_its_own_column_and_descends(tmp_path: Path) -> None:
    reports.write(tmp_path / "r1.json", shaped(100, 5_000_000, 0.2, 2_000_000))
    reports.write(tmp_path / "r2.json", shaped(50, 9_000_000, 0.8, 1_000_000))
    reports.write(tmp_path / "r3.json", shaped(70, 1_000_000, 0.5, 500_000))

    def names(key: str) -> list[str]:
        lines = run("--sort", key, tmp_path).stdout.splitlines()[1:4]
        return [line.split()[0] for line in lines]

    assert names("executed") == ["r1", "r3", "r2"]
    assert names("ms") == ["r2", "r1", "r3"]
    assert names("sleep") == ["r2", "r3", "r1"]
    assert names("irq") == ["r3", "r1", "r2"]


def test_function_share_is_over_the_executed_of_every_loaded_report(tmp_path: Path) -> None:
    base = reports.synthetic()
    reports.write(tmp_path / "a.json", with_functions(base, {"f": 30, "g": 10}))
    plain = dataclasses.replace(reports.full_report(), executed=100, hotspots=None)
    reports.write(tmp_path / "b.json", plain)
    cut = dataclasses.replace(with_functions(base, {"f": 5}), executed=60)
    reports.write(tmp_path / "c.json", cut)
    result = run("--json", "--functions", "2", tmp_path)
    assert json.loads(result.stdout)["functions"] == [
        {"function": "f", "self": 35, "share": 35 / 200, "reports": 2},
        {"function": "g", "self": 10, "share": 10 / 200, "reports": 1},
    ]
    assert f"{100 * 35 / 200:.1f}%" in run("--functions", "2", tmp_path).stdout


def test_a_name_repeated_in_one_report_counts_that_report_once(tmp_path: Path) -> None:
    base = reports.synthetic()
    assert base.hotspots is not None
    twice = dataclasses.replace(
        base,
        hotspots=dataclasses.replace(
            base.hotspots,
            functions=(FunctionRow("f", 1, 3, 3, 1), FunctionRow("f", 2, 4, 4, 1)),
        ),
    )
    reports.write(tmp_path / "a.json", twice)
    reports.write(tmp_path / "b.json", with_functions(base, {"f": 1}))
    result = run("--json", "--functions", "1", tmp_path)
    [row] = json.loads(result.stdout)["functions"]
    assert (row["function"], row["self"], row["reports"]) == ("f", 8, 2)


def test_every_file_bad_prints_an_empty_table_and_exits_1(tmp_path: Path) -> None:
    (tmp_path / "a.json").write_text("not json", encoding="utf-8")
    (tmp_path / "b.json").write_text("[]", encoding="utf-8")
    result = run(tmp_path)
    assert result.exit_code == 1
    assert result.stdout.splitlines()[-1] == "0 reports, 0 instructions"
    assert result.stderr.count("Error:") == 2
