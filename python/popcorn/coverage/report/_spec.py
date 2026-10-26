"""Report specs, `kind[:target]`, and the dispatch that writes them from one lcov file."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from popcorn.coverage.report._model import Totals, parse_lcov

KINDS = ("term", "html", "xml", "lcov")
DEFAULT_TARGETS = {"html": "htmlcov", "xml": "coverage.xml"}
WRITABLE = frozenset({"lcov"})


@dataclass(frozen=True, slots=True)
class ReportSpec:
    """One requested report.

    Attributes:
        kind: `term`, `html`, `xml` or `lcov`.
        target: The file or directory to write, None for `term`, and for an `lcov` report that
            takes its path from the caller.
    """

    kind: str
    target: Path | None = None


@dataclass(frozen=True, slots=True)
class ReportResult:
    """What `generate` produced.

    Attributes:
        totals: The counts of the report after include and omit.
        written: The files and directories written, in spec order.
    """

    totals: Totals
    written: tuple[Path, ...]


def parse_spec(text: str, base: Path | None = None) -> ReportSpec:
    """The spec of `kind[:target]`, a relative target resolved against `base` when given.

    `html` without a target writes `htmlcov`, `xml` writes `coverage.xml`.

    Raises:
        ValueError: The kind is unknown, `term` has a target, or `xml` or `html` has an empty one.
    """
    kind, colon, target = text.partition(":")
    if kind not in KINDS:
        raise ValueError(f"unknown report kind {kind!r} in {text!r}, use {', '.join(KINDS)}")
    if kind == "term" and colon:
        raise ValueError(f"the term report takes no target, not {text!r}")
    if colon and not target:
        raise ValueError(f"the {kind} report has an empty target in {text!r}")
    if not target:
        target = DEFAULT_TARGETS.get(kind, "")
    if not target:
        return ReportSpec(kind)
    path = Path(target)
    return ReportSpec(kind, base / path if base is not None else path)


def generate(
    lcov: Path,
    specs: Sequence[ReportSpec],
    *,
    include: Sequence[str] = (),
    omit: Sequence[str] = (),
    root: Path,
) -> ReportResult:
    """Parse `lcov` once, apply `include` and `omit`, and write every report of `specs`.

    Every spec is checked before the first report is written, so a refused list writes nothing.
    An `lcov` report holds the bytes of `lcov` when no filter is given, else the filtered
    tracefile as UTF-8.

    Raises:
        ValueError: `lcov` is not a tracefile, or an `lcov` spec has no target.
        NotImplementedError: A spec of another kind than `lcov`.
        OSError: A target cannot be written.
    """
    targets: list[Path] = []
    for spec in specs:
        if spec.kind not in WRITABLE:
            raise NotImplementedError(f"the {spec.kind} report is not available")
        if spec.target is None:
            raise ValueError("an lcov report needs a target path")
        targets.append(spec.target)
    data = lcov.read_bytes()
    report = parse_lcov(data.decode())
    filtered = bool(include or omit)
    if filtered:
        report = report.filtered(include, omit, root)
    for target in targets:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(report.to_lcov().encode() if filtered else data)
    return ReportResult(report.totals(), tuple(targets))
