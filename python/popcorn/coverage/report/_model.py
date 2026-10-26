"""The lcov data model: parse, filter, count and serialize an lcov tracefile.

Source paths follow one rule. A path is resolved against `root`, made absolute and normalized at
entry: `resolve` returns an absolute path as it is and a relative one under `root`. A relative
path is its own display name, an absolute path under `root` displays relative to it.

Include and omit patterns are globs matched with `PurePath.full_match`, so `**` spans directories.
A pattern is tried against the path as written and against its path relative to the root, for an
absolute path under the root. A relative path is also tried in its resolved form against an
absolute pattern. Use `**/tests/**` to select a directory at any depth. The coverage.py idiom
`*/tests/*` selects nothing.
"""

from __future__ import annotations

import os
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path, PurePath
from types import MappingProxyType

__all__ = [
    "BranchRecord",
    "Counts",
    "FunctionRecord",
    "LcovFile",
    "LcovFormatError",
    "LcovReport",
    "Totals",
    "display_name",
    "matches",
    "parse_lcov",
    "resolve",
]


class LcovFormatError(ValueError):
    """The text is not an lcov tracefile. The message starts with the 1-based line number."""


@dataclass(frozen=True, slots=True)
class Counts:
    """Items found and items hit."""

    found: int = 0
    hit: int = 0

    def __add__(self, other: Counts) -> Counts:
        return Counts(self.found + other.found, self.hit + other.hit)

    @property
    def percent(self) -> float:
        """Percent hit, 100.0 when nothing was found."""
        return 100.0 if self.found == 0 else 100.0 * self.hit / self.found


@dataclass(frozen=True, slots=True)
class Totals:
    """Line, function and branch counts."""

    lines: Counts = Counts()
    functions: Counts = Counts()
    branches: Counts = Counts()

    def __add__(self, other: Totals) -> Totals:
        return Totals(
            self.lines + other.lines,
            self.functions + other.functions,
            self.branches + other.branches,
        )


def below(totals: Totals, threshold: float) -> bool:
    """True when the line coverage of `totals` is under `threshold` percent.

    The comparison is exact on the decimal text of `threshold`, no percentage is rounded first.
    Nothing found counts as 0 percent, so it is below any positive threshold.
    """
    lines = totals.lines
    if lines.found == 0:
        return threshold > 0
    return lines.hit * 100 < Fraction(str(threshold)) * lines.found


def truncated_percent(counts: Counts) -> str:
    """The percent hit of `counts` with two decimals, cut and never rounded up.

    Nothing found gives `0.00`, and so does one hit in more than 10000 found lines.
    """
    hundredths = 0 if counts.found == 0 else counts.hit * 10000 // counts.found
    return f"{hundredths // 100}.{hundredths % 100:02d}"


@dataclass(frozen=True, slots=True)
class FunctionRecord:
    """A function: `FN` start and end line (None when the file gives no end) and `FNDA` hits."""

    name: str
    start: int
    end: int | None
    hits: int


@dataclass(frozen=True, slots=True)
class BranchRecord:
    """A `BRDA` record, `taken` is None for a branch that was never evaluated."""

    line: int
    block: int
    branch: int
    taken: int | None


@dataclass(frozen=True, slots=True)
class LcovFile:
    """One `SF` record: the test name, the line hit counts, the functions and the branches."""

    path: str
    test_name: str
    lines: Mapping[int, int] = field(hash=False)
    functions: tuple[FunctionRecord, ...]
    branches: tuple[BranchRecord, ...]

    def totals(self) -> Totals:
        """The counts the file's own LF/LH, FNF/FNH and BRF/BRH hold."""
        return Totals(
            Counts(len(self.lines), sum(1 for hits in self.lines.values() if hits != 0)),
            Counts(
                len(self.functions), sum(1 for function in self.functions if function.hits != 0)
            ),
            Counts(len(self.branches), sum(1 for branch in self.branches if branch.taken)),
        )


def _root(root: Path) -> PurePath:
    return PurePath(os.path.abspath(root))


def _normal(path: str) -> PurePath:
    pure = PurePath(path)
    return PurePath(os.path.normpath(pure)) if pure.is_absolute() else pure


def resolve(sf: str, root: Path) -> Path:
    """The absolute location of an `SF` path: itself when absolute, else under `root`."""
    path = Path(sf)
    return path if path.is_absolute() else Path(_root(root)) / path


def display_name(sf: str, root: Path) -> str:
    """Display name: a relative path as written, an absolute one under `root` made relative."""
    path = _normal(sf)
    base = _root(root)
    if path.is_absolute() and path.is_relative_to(base):
        return path.relative_to(base).as_posix()
    return sf


def matches(path: str, patterns: Iterable[str], root: Path) -> bool:
    """True when a glob of `patterns` matches `path` as written or relative to `root`."""
    pure = _normal(path)
    base = _root(root)
    forms: list[tuple[PurePath, bool]] = [(pure, False)]
    if not pure.is_absolute():
        forms.append((base / pure, True))
    elif pure.is_relative_to(base):
        forms.append((pure.relative_to(base), False))
    for pattern in patterns:
        absolute = PurePath(pattern).is_absolute()
        if any((absolute or not resolved) and form.full_match(pattern) for form, resolved in forms):
            return True
    return False


def _number(text: str, number: int, what: str) -> int:
    try:
        return int(text)
    except ValueError:
        raise LcovFormatError(f"line {number}: bad {what} {text!r}") from None


def _function(rest: str, number: int) -> tuple[str, int, int | None]:
    parts = rest.split(",", 2)
    if len(parts) == 3 and parts[1].isascii() and parts[1].isdigit():
        return parts[2], _number(parts[0], number, "FN start"), int(parts[1])
    if len(parts) < 2:
        raise LcovFormatError(f"line {number}: bad FN record {rest!r}")
    return rest.split(",", 1)[1], _number(parts[0], number, "FN start"), None


def parse_lcov(text: str) -> LcovReport:
    """Parses an lcov tracefile.

    Counters `LF`, `LH`, `FNF`, `FNH`, `BRF` and `BRH` are recomputed from the records. Tags
    outside the supported set are skipped and `DA` checksums are dropped. Repeated `DA`, `FNDA`
    and `BRDA` records of one item in a block are summed, as lcov does. Each `SF` block is its own
    `LcovFile`, so a path that appears in two blocks gives two files and `totals` counts both.

    Raises:
        LcovFormatError: a record outside an `SF` block, a malformed record, or a block that does
            not end with `end_of_record`.
    """
    files: list[LcovFile] = []
    has_branch_summary = False
    test_name = ""
    path: str | None = None
    lines: dict[int, int] = {}
    starts: list[tuple[str, int, int | None]] = []
    hits: dict[str, int] = {}
    branches: dict[tuple[int, int, int], int | None] = {}
    number = 0
    for number, raw in enumerate(text.removesuffix("\n").split("\n"), 1):
        line = raw.rstrip("\r")
        if not line.strip():
            continue
        tag, _, rest = line.partition(":")
        if tag == "TN":
            if path is not None:
                raise LcovFormatError(f"line {number}: TN inside a record")
            test_name = rest
        elif tag == "SF":
            if path is not None:
                raise LcovFormatError(f"line {number}: SF before end_of_record")
            path = rest
        elif path is None:
            raise LcovFormatError(f"line {number}: {tag} outside an SF record")
        elif tag == "FN":
            starts.append(_function(rest, number))
        elif tag == "FNDA":
            count, _, name = rest.partition(",")
            hits[name] = hits.get(name, 0) + _number(count, number, "FNDA count")
        elif tag == "DA":
            fields = rest.split(",")
            if len(fields) < 2:
                raise LcovFormatError(f"line {number}: bad DA record {rest!r}")
            at = _number(fields[0], number, "DA line")
            lines[at] = lines.get(at, 0) + _number(fields[1], number, "DA count")
        elif tag == "BRDA":
            fields = rest.split(",")
            if len(fields) != 4:
                raise LcovFormatError(f"line {number}: bad BRDA record {rest!r}")
            taken = None if fields[3] == "-" else _number(fields[3], number, "BRDA taken")
            key = (
                _number(fields[0], number, "BRDA line"),
                _number(fields[1], number, "BRDA block"),
                _number(fields[2], number, "BRDA branch"),
            )
            before = branches.get(key)
            branches[key] = taken if before is None else before + (taken or 0)
        elif tag in ("BRF", "BRH"):
            has_branch_summary = True
        elif tag == "end_of_record":
            functions = tuple(
                FunctionRecord(name, start, end, hits.get(name, 0)) for name, start, end in starts
            )
            records = tuple(BranchRecord(*key, taken) for key, taken in branches.items())
            files.append(LcovFile(path, test_name, MappingProxyType(lines), functions, records))
            test_name, path, lines, starts, hits, branches = "", None, {}, [], {}, {}
    if path is not None:
        raise LcovFormatError(f"line {number}: {path} has no end_of_record")
    return LcovReport(tuple(files), has_branch_summary)


@dataclass(frozen=True, slots=True)
class LcovReport:
    """The files of an lcov tracefile.

    `has_branch_summary` is True when the parsed text carried `BRF` or `BRH` lines, which
    `to_lcov` then emits for every file, as the writer does when branch coverage is on.
    """

    files: tuple[LcovFile, ...]
    has_branch_summary: bool = False

    def totals(self) -> Totals:
        """The counts summed over every file."""
        return sum((file.totals() for file in self.files), Totals())

    def filtered(self, include: Sequence[str], omit: Sequence[str], root: Path) -> LcovReport:
        """The files that match `include` (all when empty) and no pattern of `omit`.

        Patterns are matched as `matches` does, relative paths against `root`.
        """
        kept = tuple(
            file
            for file in self.files
            if (not include or matches(file.path, include, root))
            and not matches(file.path, omit, root)
        )
        return LcovReport(kept, self.has_branch_summary)

    def to_lcov(self) -> str:
        """The tracefile text, in the record order of the native writer."""
        out: list[str] = []
        for file in self.files:
            out.append(f"TN:{file.test_name}\nSF:{file.path}\n")
            for function in file.functions:
                span = (
                    f"{function.start},"
                    if function.end is None
                    else f"{function.start},{function.end},"
                )
                out.append(f"FN:{span}{function.name}\n")
            for function in file.functions:
                out.append(f"FNDA:{function.hits},{function.name}\n")
            counts = file.totals()
            out.append(f"FNF:{counts.functions.found}\nFNH:{counts.functions.hit}\n")
            for branch in file.branches:
                taken = "-" if branch.taken is None else str(branch.taken)
                out.append(f"BRDA:{branch.line},{branch.block},{branch.branch},{taken}\n")
            if self.has_branch_summary:
                out.append(f"BRF:{counts.branches.found}\nBRH:{counts.branches.hit}\n")
            for number in sorted(file.lines):
                out.append(f"DA:{number},{file.lines[number]}\n")
            out.append(f"LF:{counts.lines.found}\nLH:{counts.lines.hit}\nend_of_record\n")
        return "".join(out)
