"""popcorn.coverage.report terminal table: the formatting of `render_term`."""

from __future__ import annotations

from pathlib import Path

import pytest
from popcorn.coverage.report import parse_lcov, render_term

ABSOLUTE_INSIDE = "/work/proj/src/driver.c"
ABSOLUTE_OUTSIDE = "/opt/sdk/lib/util.c"
RELATIVE = "tests/firmware/shared_exit.S"

TRACEFILE = f"""TN:
SF:{ABSOLUTE_INSIDE}
FN:1,4,add
FN:6,9,sub
FNDA:3,add
FNDA:0,sub
FNF:2
FNH:1
DA:1,3
DA:2,3
DA:7,0
DA:8,0
LF:4
LH:2
BRDA:2,0,0,1
BRDA:2,0,1,-
BRF:2
BRH:1
end_of_record
TN:
SF:{ABSOLUTE_OUTSIDE}
DA:1,5
LF:1
LH:1
BRF:0
BRH:0
end_of_record
TN:
SF:{RELATIVE}
FN:3,5,start
FNDA:1,start
FNF:1
FNH:1
DA:3,1
DA:4,1
DA:5,1
LF:3
LH:3
BRF:0
BRH:0
end_of_record
"""

GOLDEN = """\
Name                          Lines  Hit   Cover  Funcs   Cover  Branches  Cover
--------------------------------------------------------------------------------
/opt/sdk/lib/util.c               1    1  100.0%      0       -         0      -
src/driver.c                      4    2   50.0%      2   50.0%         2  50.0%
tests/firmware/shared_exit.S      3    3  100.0%      1  100.0%         0      -
--------------------------------------------------------------------------------
TOTAL                             8    6   75.0%      3   66.6%         2  50.0%"""


def test_golden_table_with_absolute_inside_outside_and_relative_paths() -> None:
    text = render_term(parse_lcov(TRACEFILE), root=Path("/work/proj"))
    assert text == GOLDEN


def test_without_a_root_paths_are_shown_as_written() -> None:
    text = render_term(parse_lcov(TRACEFILE), root=None)
    assert ABSOLUTE_INSIDE in text
    assert "src/driver.c " not in text.replace(ABSOLUTE_INSIDE, "")


def test_every_line_has_the_width_of_the_longest_name() -> None:
    lines = render_term(parse_lcov(TRACEFILE), root=Path("/work/proj")).splitlines()
    assert len({len(line) for line in lines}) == 1


def test_an_empty_report_says_so() -> None:
    assert render_term(parse_lcov(""), root=Path("/work")) == "no firmware coverage"


def counted_report(found: int, hit: int) -> str:
    records = "".join(f"DA:{n},{1 if n <= hit else 0}\n" for n in range(1, found + 1))
    return f"TN:\nSF:/work/proj/a.c\n{records}LF:{found}\nLH:{hit}\nend_of_record\n"


@pytest.mark.parametrize(
    ("found", "hit", "cover"),
    [
        (10000, 9999, "99.9%"),
        (10000, 1, "0.1%"),
        (10000, 10000, "100.0%"),
        (10000, 0, "0.0%"),
        (3, 2, "66.6%"),
        (8, 1, "12.5%"),
    ],
)
def test_a_partial_percent_is_never_rounded_to_the_extremes(
    found: int, hit: int, cover: str
) -> None:
    text = render_term(parse_lcov(counted_report(found, hit)), root=Path("/work/proj"))
    assert text.splitlines()[-1].split()[3] == cover
