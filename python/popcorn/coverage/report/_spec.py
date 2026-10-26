"""Report specs, `kind[:target]`, and the dispatch that writes them from one lcov file."""

from __future__ import annotations

import os
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from popcorn.coverage.report._cobertura import render_cobertura
from popcorn.coverage.report._model import Totals, parse_lcov
from popcorn.coverage.report._term import render_term

KINDS = ("term", "html", "xml", "lcov")
DEFAULT_TARGETS = {"html": "htmlcov", "xml": "coverage.xml"}
WRITABLE = frozenset({"term", "lcov", "xml"})


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
        term: The text of the `term` report, None when no spec asked for it.
    """

    totals: Totals
    written: tuple[Path, ...]
    term: str | None = None


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


def _write_atomic(target: Path, data: bytes) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    scratch = target.with_name(f".{target.name}.{os.getpid()}.tmp")
    try:
        scratch.write_bytes(data)
        scratch.replace(target)
    except BaseException:
        scratch.unlink(missing_ok=True)
        raise


def generate(
    lcov: Path,
    specs: Sequence[ReportSpec],
    *,
    include: Sequence[str] = (),
    omit: Sequence[str] = (),
    root: Path,
    timestamp: int | None = None,
) -> ReportResult:
    """Parse `lcov` once, apply `include` and `omit`, and write every report of `specs`.

    Every spec is checked before the first report is written, so a refused list writes nothing.
    The `term` report is returned in `ReportResult.term`, `generate` never prints it. An `lcov`
    report holds the bytes of `lcov` when no filter is given, else the filtered tracefile as UTF-8.
    An `xml` report is the Cobertura file of `render_cobertura`, written through a temporary file
    in its directory and renamed, with `timestamp` in milliseconds since the epoch, the current
    time when None.

    Raises:
        ValueError: `lcov` is not a tracefile, or an `lcov` or `xml` spec has no target.
        NotImplementedError: A spec of kind `html`.
        OSError: A target cannot be written.
    """
    files: list[ReportSpec] = []
    for spec in specs:
        if spec.kind not in WRITABLE:
            raise NotImplementedError(f"the {spec.kind} report is not available")
        if spec.kind != "term":
            if spec.target is None:
                raise ValueError(f"an {spec.kind} report needs a target path")
            files.append(spec)
    data = lcov.read_bytes()
    report = parse_lcov(data.decode())
    filtered = bool(include or omit)
    if filtered:
        report = report.filtered(include, omit, root)
    written: list[Path] = []
    for spec in files:
        target = spec.target
        assert target is not None
        if spec.kind == "xml":
            stamp = int(time.time() * 1000) if timestamp is None else timestamp
            xml = render_cobertura(report, root=root, timestamp=stamp)
            _write_atomic(target, xml.encode())
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(report.to_lcov().encode() if filtered else data)
        written.append(target)
    term = render_term(report, root=root) if any(spec.kind == "term" for spec in specs) else None
    return ReportResult(report.totals(), tuple(written), term)
