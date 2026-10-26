"""popcorn.coverage.report: parse, count, filter and serialize lcov, byte-exact round trips."""

from __future__ import annotations

import re
from pathlib import Path

import pytest
from popcorn.coverage import (
    BlockHit,
    CodeRange,
    Coverage,
    CoverageGroup,
    EdgeHit,
    ImageRef,
    LcovFormatError,
    parse_lcov,
)
from popcorn.coverage.report import (
    Counts,
    Totals,
    below,
    display_name,
    matches,
    resolve,
    truncated_percent,
)

PREBUILT = Path(__file__).parents[2] / "firmware" / "obs" / "prebuilt"
LINES = "tests/firmware/obs/lines.c"
CUT = "tests/firmware/obs/cut.c"
ROOT = Path("/home/u/proj")


def session() -> Coverage:
    lines = ImageRef.from_elf(PREBUILT / "lines.elf")
    cut = ImageRef.from_elf(PREBUILT / "cut.elf")
    return Coverage.from_parts(
        [
            CoverageGroup(
                images=(lines,),
                ranges=(
                    CodeRange("clamp", 0x0800_0008, 0x10),
                    CodeRange("idle", 0x0800_0020, 0x10),
                ),
                blocks=(BlockHit(0x0800_0008, 4, 2), BlockHit(0x0800_0014, 4, 1)),
                edges=(EdgeHit(0x0800_0008, 0x0800_0014, 1),),
            ),
            CoverageGroup(
                images=(cut,),
                ranges=(CodeRange("pick", 0x0800_002C, 0x10),),
                blocks=(BlockHit(0x0800_002C, 4, 3),),
                edges=(),
            ),
        ]
    )


def exported(tmp_path: Path, *, branches: bool) -> str:
    out = tmp_path / "cov.info"
    session().export_lcov(out, branches=branches, test_name="t")
    return out.read_text()


def own_counters(text: str) -> dict[str, int]:
    sums: dict[str, int] = {}
    for tag, value in re.findall(r"^(LF|LH|FNF|FNH|BRF|BRH):(\d+)$", text, re.MULTILINE):
        sums[tag] = sums.get(tag, 0) + int(value)
    return sums


@pytest.mark.parametrize("branches", [True, False])
def test_writer_output_round_trips_byte_for_byte(tmp_path: Path, branches: bool) -> None:
    text = exported(tmp_path, branches=branches)
    report = parse_lcov(text)
    assert [file.path for file in report.files] == [CUT, LINES]
    assert report.has_branch_summary is branches
    assert report.to_lcov() == text
    assert ("BRDA:" in text) is branches


@pytest.mark.parametrize("branches", [True, False])
def test_a_file_with_no_function_and_no_branch_round_trips(branches: bool) -> None:
    summary = "BRF:0\nBRH:0\n" if branches else ""
    text = f"TN:\nSF:x.c\nFNF:0\nFNH:0\n{summary}DA:1,1\nDA:2,0\nLF:2\nLH:1\nend_of_record\n"
    report = parse_lcov(text)
    assert report.to_lcov() == text
    assert report.files[0].functions == ()
    assert report.files[0].branches == ()


def test_a_function_that_never_ran_round_trips_with_zero_hits(tmp_path: Path) -> None:
    text = exported(tmp_path, branches=True)
    assert re.search(r"^FNDA:0,", text, re.MULTILINE)
    assert parse_lcov(text).to_lcov() == text


def test_repeated_records_of_one_item_are_summed() -> None:
    text = (
        "SF:a.c\nFN:1,5,f\nFNDA:3,f\nFNDA:0,f\nDA:7,3\nDA:7,0\nDA:8,0\nDA:8,0\n"
        "BRDA:7,0,0,-\nBRDA:7,0,0,2\nBRDA:7,0,1,0\nBRDA:7,0,1,0\nend_of_record\n"
    )
    file = parse_lcov(text).files[0]
    assert dict(file.lines) == {7: 3, 8: 0}
    assert file.functions[0].hits == 3
    assert [(b.branch, b.taken) for b in file.branches] == [(0, 2), (1, 0)]


def test_a_path_in_two_blocks_is_two_files_and_counts_twice() -> None:
    block = "SF:a.c\nDA:1,1\nend_of_record\n"
    report = parse_lcov(block + block)
    assert len(report.files) == 2
    assert report.totals().lines.found == 2


def test_file_totals_is_a_method_and_the_file_is_hashable() -> None:
    file = parse_lcov("SF:a.c\nDA:1,1\nDA:2,0\nend_of_record\n").files[0]
    assert file.totals().lines.found == 2
    assert file.totals().lines.hit == 1
    assert isinstance(hash(file), int)
    with pytest.raises(TypeError):
        file.lines[3] = 1  # type: ignore[index]


def test_non_ascii_digits_are_not_an_end_line() -> None:
    function = parse_lcov("SF:a.c\nFN:1,\u00b2,f\nend_of_record\n").files[0].functions[0]
    assert (function.end, function.name) == (None, "\u00b2,f")


def test_content_is_not_split_or_trimmed() -> None:
    path = "a\u2028 b\x0c.c "
    text = f"TN: t \nSF:{path}\nDA:1,1\nend_of_record\n"
    report = parse_lcov(text)
    assert report.files[0].path == path
    assert report.files[0].test_name == " t "
    assert report.to_lcov().startswith(f"TN: t \nSF:{path}\n")
    assert len(parse_lcov(text.replace("\n", "\r\n")).files) == 1


def test_a_relative_or_unnormalized_root_is_made_absolute() -> None:
    cwd = Path.cwd()
    assert resolve("x.c", Path(".")) == cwd / "x.c"
    assert display_name(str(cwd / "x.c"), Path(".")) == "x.c"
    assert matches(str(cwd / "src" / "x.c"), ["src/**"], Path("."))
    assert display_name("/r/../r/x.c", Path("/r")) == "x.c"
    assert display_name("/r/../x.c", Path("/r")) == "/r/../x.c"


def test_an_empty_export_round_trips() -> None:
    assert parse_lcov("").to_lcov() == ""
    assert parse_lcov("").totals().lines.found == 0


@pytest.mark.parametrize("branches", [True, False])
def test_totals_equal_the_counters_of_the_file(tmp_path: Path, branches: bool) -> None:
    text = exported(tmp_path, branches=branches)
    totals = parse_lcov(text).totals()
    counters = own_counters(text)
    assert (totals.lines.found, totals.lines.hit) == (counters["LF"], counters["LH"])
    assert (totals.functions.found, totals.functions.hit) == (counters["FNF"], counters["FNH"])
    assert (totals.branches.found, totals.branches.hit) == (
        counters.get("BRF", 0),
        counters.get("BRH", 0),
    )
    assert totals.lines.found > 0 and totals.functions.hit > 0
    assert (totals.branches.found > 0) is branches


def test_unevaluated_branches_count_as_found_and_not_hit() -> None:
    text = "SF:a.c\nBRDA:3,0,0,-\nBRDA:3,0,1,2\nBRF:2\nBRH:1\nend_of_record\n"
    totals = parse_lcov(text).totals()
    assert (totals.branches.found, totals.branches.hit) == (2, 1)
    assert totals.branches.percent == 50.0
    assert parse_lcov(text).to_lcov().count("BRDA:3,0,0,-") == 1


def test_counters_are_recomputed_from_the_records() -> None:
    text = "SF:a.c\nFN:1,2,f\nFNDA:1,f\nFNF:9\nFNH:9\nDA:1,1\nLF:9\nLH:9\nend_of_record\n"
    out = parse_lcov(text).to_lcov()
    assert "FNF:1\nFNH:1\n" in out
    assert "LF:1\nLH:1\n" in out


def test_function_with_no_end_and_a_comma_in_the_name() -> None:
    report = parse_lcov("SF:a.c\nFN:4,g\nFN:7,9,h<int, int>\nFNDA:2,h<int, int>\nend_of_record\n")
    first, second = report.files[0].functions
    assert (first.name, first.start, first.end, first.hits) == ("g", 4, None, 0)
    assert (second.name, second.start, second.end, second.hits) == ("h<int, int>", 7, 9, 2)
    assert "FN:4,g\n" in report.to_lcov()


def test_percent_of_nothing_is_one_hundred() -> None:
    assert parse_lcov("").totals().lines.percent == 100.0


@pytest.mark.parametrize(
    ("text", "message"),
    [
        ("DA:1,1\n", "line 1: DA outside an SF record"),
        ("SF:a.c\nDA:1,1\n", "line 2: a.c has no end_of_record"),
        ("SF:a.c\nSF:b.c\n", "line 2: SF before end_of_record"),
        ("SF:a.c\n\nDA:x,1\nend_of_record\n", "line 3: bad DA line 'x'"),
        ("SF:a.c\nBRDA:1,2,3\nend_of_record\n", "line 2: bad BRDA record '1,2,3'"),
        ("end_of_record\n", "line 1: end_of_record outside an SF record"),
    ],
)
def test_malformed_input_names_the_line(text: str, message: str) -> None:
    with pytest.raises(LcovFormatError, match=re.escape(message)) as raised:
        parse_lcov(text)
    assert isinstance(raised.value, ValueError)


def report_of(*paths: str) -> str:
    return "".join(f"SF:{path}\nDA:1,1\nend_of_record\n" for path in paths)


def kept(include: list[str], omit: list[str], *paths: str) -> list[str]:
    report = parse_lcov(report_of(*paths)).filtered(include, omit, ROOT)
    return [file.path for file in report.files]


def test_double_star_selects_a_directory_at_any_depth() -> None:
    paths = ("/home/u/proj/a/tests/x.c", "tests/y.c", "/elsewhere/tests/z.c", "/home/u/proj/a/s.c")
    assert kept(["**/tests/**"], [], *paths) == list(paths[:3])
    assert kept([], ["**/tests/**"], *paths) == [paths[3]]


def test_single_star_idiom_of_coverage_py_matches_nothing() -> None:
    assert kept(["*/tests/*"], [], "/home/u/proj/tests/a.c", "tests/a.c") == []


def test_root_relative_pattern_matches_an_absolute_path_under_the_root() -> None:
    assert kept(["src/*.c"], [], "/home/u/proj/src/a.c", "/other/src/a.c") == [
        "/home/u/proj/src/a.c"
    ]


def test_absolute_pattern_matches_a_path_as_written() -> None:
    assert kept(["/home/u/proj/src/**"], [], "/home/u/proj/src/a.c", "/x/a.c") == [
        "/home/u/proj/src/a.c"
    ]


def test_relative_sf_against_relative_and_absolute_patterns() -> None:
    assert kept(["tests/firmware/**"], [], LINES, CUT) == [LINES, CUT]
    assert kept(["/home/u/proj/tests/firmware/obs/*.c"], [], LINES) == [LINES]
    assert kept(["/home/u/proj/tests/firmware/obs/*.c"], [], "/home/u/proj/other/a.c") == []
    assert kept([], ["/home/u/proj/tests/**"], LINES, "src/a.c") == ["src/a.c"]


def test_a_relative_sf_does_not_match_a_relative_pattern_through_the_root() -> None:
    assert kept(["home/**"], [], "src/a.c") == []
    assert kept(["**/proj/**"], [], "src/a.c") == []


def test_omit_wins_over_include() -> None:
    paths = ("src/a.c", "src/gen/b.c")
    assert kept(["src/**"], ["src/gen/**"], *paths) == ["src/a.c"]
    assert kept(["src/gen/**"], ["src/**"], *paths) == []


def test_filtering_keeps_the_branch_summary_flag_and_serializes() -> None:
    text = "SF:a.c\nBRDA:1,0,0,1\nBRF:1\nBRH:1\nend_of_record\nSF:b.c\nDA:1,0\nend_of_record\n"
    out = parse_lcov(text).filtered([], ["b.c"], ROOT)
    assert out.has_branch_summary
    assert (
        out.to_lcov()
        == "TN:\nSF:a.c\nFNF:0\nFNH:0\nBRDA:1,0,0,1\nBRF:1\nBRH:1\nLF:0\nLH:0\nend_of_record\n"
    )


def test_resolve_and_display_name_follow_the_root_not_the_process_directory() -> None:
    assert resolve("a/b.c", ROOT) == ROOT / "a" / "b.c"
    assert resolve("/x/b.c", ROOT) == Path("/x/b.c")
    assert display_name("a/b.c", ROOT) == "a/b.c"
    assert display_name("/home/u/proj/a/b.c", ROOT) == "a/b.c"
    assert display_name("/home/u/projx/b.c", ROOT) == "/home/u/projx/b.c"
    assert matches("a/b.c", ["**/b.c"], ROOT)


def lines_of(found: int, hit: int) -> Totals:
    return Totals(lines=Counts(found, hit))


def test_below_is_false_at_the_threshold_and_true_under_it() -> None:
    assert not below(lines_of(4, 3), 75)
    assert below(lines_of(4, 3), 75.01)
    assert not below(lines_of(3, 3), 100)
    assert below(lines_of(3, 2), 100)
    assert not below(lines_of(7, 0), 0)


def test_below_compares_the_exact_ratio_not_a_rounded_one() -> None:
    totals = lines_of(100000, 89996)
    assert below(totals, 90)
    assert not below(totals, 89.996)
    assert below(totals, 89.997)
    assert round(totals.lines.percent, 2) == 90.0


def test_the_printed_percent_is_cut_never_rounded_up() -> None:
    assert truncated_percent(Counts(100000, 89996)) == "89.99"
    assert truncated_percent(Counts(3, 2)) == "66.66"
    assert truncated_percent(Counts(3, 3)) == "100.00"
    assert truncated_percent(Counts(7, 0)) == "0.00"


def test_nothing_found_is_zero_percent() -> None:
    assert below(Totals(), 0.01)
    assert not below(Totals(), 0)
    assert truncated_percent(Counts()) == "0.00"
