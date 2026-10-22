import re
from pathlib import Path

import pytest

ROOT = Path(__file__).parents[2]
BASES = (ROOT / "tests/docs", ROOT / "examples")
INCLUDE = re.compile(r'^--8<-- "([^":]+):(\d+):(\d+)"$', re.MULTILINE)
CPP_START = {
    "stm32u575/native_plugin/plugin/rng.cpp": "POPCORN_PLUGIN",
    "stm32u575/native_plugin/plugin/bind.cpp": "NB_MODULE",
}


def includes() -> list[tuple[Path, str, int, int]]:
    found: list[tuple[Path, str, int, int]] = []
    for page in sorted((ROOT / "docs").rglob("*.md")):
        for name, first, last in INCLUDE.findall(page.read_text()):
            found.append((page, name, int(first), int(last)))
    return found


def source(name: str) -> list[str]:
    path = next(base / name for base in BASES if (base / name).exists())
    return path.read_text().split("\n")


def balanced(lines: list[str]) -> bool:
    text = "\n".join(lines)
    return text.count("{") == text.count("}") and text.count("(") == text.count(")")


def check_markdown(lines: list[str], first: int, last: int) -> list[str]:
    problems: list[str] = []
    if first > 1 and lines[first - 2].strip():
        problems.append("starts mid-block")
    if last < len(lines) and lines[last].strip():
        problems.append("ends mid-block")
    if sum(line.startswith("```") for line in lines[first - 1 : last]) % 2:
        problems.append("unbalanced fence")
    return problems


def check_cpp(name: str, lines: list[str], first: int, last: int) -> list[str]:
    problems: list[str] = []
    chosen = lines[first - 1 : last]
    if not chosen[0].lstrip().startswith(CPP_START[name]):
        problems.append(f"does not start with {CPP_START[name]}")
    if not chosen[-1].rstrip().endswith(("}", "};", ";")):
        problems.append("ends mid-statement")
    if not balanced(chosen):
        problems.append("unbalanced braces or parentheses")
    return problems


def problems_of(name: str, first: int, last: int) -> list[str]:
    lines = source(name)
    if name.endswith(".md"):
        return check_markdown(lines, first, last)
    if name.endswith(".cpp"):
        return check_cpp(name, lines, first, last)
    return [f"no range check for {name}"]


@pytest.mark.parametrize(
    ("page", "name", "first", "last"),
    includes(),
    ids=lambda value: str(value) if isinstance(value, str | int) else value.name,
)
def test_a_line_range_include_covers_whole_blocks(
    page: Path, name: str, first: int, last: int
) -> None:
    assert problems_of(name, first, last) == []


def test_the_guard_rejects_the_range_that_stopped_mid_statement() -> None:
    assert problems_of("stm32u575/native_plugin/plugin/bind.cpp", 9, 15) == [
        "ends mid-statement",
        "unbalanced braces or parentheses",
    ]
