"""Report specs, `kind[:target]`, and the dispatch that writes them from one lcov file."""

from __future__ import annotations

import contextlib
import os
import shutil
import tempfile
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from popcorn.coverage._coverage import unmodelled_path
from popcorn.coverage.report._cobertura import render_cobertura
from popcorn.coverage.report._genhtml import (
    GENHTML_TIMEOUT,
    find_genhtml,
    prepare_output,
    run_genhtml,
)
from popcorn.coverage.report._html import render_html
from popcorn.coverage.report._model import (
    LcovReport,
    Totals,
    UnmodelledUses,
    check_mode,
    dump_unmodelled,
    parse_lcov,
    parse_unmodelled,
)
from popcorn.coverage.report._term import render_term

KINDS = ("term", "html", "xml", "lcov")
DEFAULT_TARGETS = {"html": "htmlcov", "xml": "coverage.xml"}


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
        problems: One line for each report that was not written and does not fail the others, in
            spec order.
    """

    totals: Totals
    written: tuple[Path, ...]
    term: str | None = None
    problems: tuple[str, ...] = ()


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


def use_genhtml(report: LcovReport) -> bool:
    """Whether genhtml writes the `html` report of `report`.

    That is when genhtml is installed and `report` holds no line that accessed unmodelled
    registers, which only the built-in renderer marks. Otherwise the built-in renderer writes it.
    """
    return find_genhtml() is not None and not report.unmodelled


def _protected(root: Path, lcov: Path) -> tuple[tuple[str, Path], ...]:
    held = [("the rootdir", root), ("the working directory", Path.cwd())]
    with contextlib.suppress(RuntimeError):
        held.append(("the home directory", Path.home()))
    held.append(("the coverage data", lcov))
    return tuple(held)


def _discard(target: Path, *, keep: bool) -> None:
    if not keep:
        shutil.rmtree(target, ignore_errors=True)
        return
    for entry in target.iterdir():
        if entry.is_dir() and not entry.is_symlink():
            shutil.rmtree(entry, ignore_errors=True)
        else:
            entry.unlink(missing_ok=True)


def _write_html(
    report: LcovReport, lcov: Path, target: Path, rewritten: bool, root: Path
) -> str | None:
    was_empty_dir = target.is_dir() and not target.is_symlink() and not any(target.iterdir())
    reason = prepare_output(target, protect=_protected(root, lcov))
    if reason is not None:
        return f"html report not written, {reason}"
    problem: str | None = None
    written = False
    try:
        if not use_genhtml(report):
            render_html(report, target, root=root)
        else:
            with tempfile.TemporaryDirectory(prefix="popcorn-html-") as scratch:
                source = lcov
                if rewritten:
                    source = Path(scratch) / "filtered.info"
                    source.write_text(report.to_lcov())
                done = run_genhtml(source, target, root=root)
            if done.timed_out:
                problem = f"html report not written, genhtml ran past {GENHTML_TIMEOUT:g} s"
            elif done.returncode != 0:
                detail = done.stderr.strip()
                problem = f"html report not written, genhtml exited {done.returncode}: {detail}"
        written = problem is None
    finally:
        if not written:
            _discard(target, keep=was_empty_dir)
    return problem


def generate(
    lcov: Path,
    specs: Sequence[ReportSpec],
    *,
    include: Sequence[str] = (),
    omit: Sequence[str] = (),
    root: Path,
    timestamp: int | None = None,
    unmodelled: str = "hit",
) -> ReportResult:
    """Parse `lcov` once, apply `include` and `omit`, and write every report of `specs`.

    Every spec is checked before the first report is written, so a refused list writes nothing.
    The `term` report is returned in `ReportResult.term`, `generate` never prints it. An `lcov`
    report holds the bytes of `lcov` when no filter is given, else the filtered tracefile as UTF-8.
    An `xml` report is the Cobertura file of `render_cobertura`, written through a temporary file
    in its directory and renamed, with `timestamp` in milliseconds since the epoch, the current
    time when None.

    An `html` report is written by genhtml from LCOV 2.0 or later when it is installed, else by
    `render_html`, into a missing or empty directory or one that holds an earlier html report of
    either writer, which is emptied first. A symbolic link, a directory of other content, a report
    directory that holds a `.git` entry, and a directory that is or contains the rootdir, the
    working directory, the home directory or `lcov` are left untouched. Each of those, and a
    genhtml run that fails, times out or finds a source line past the end of a source file edited
    after the build, adds a line to `ReportResult.problems` and leaves the other reports written.
    After a failed run a directory the call created or replaced a report in is removed, and a
    directory that was empty before the call stays, emptied of what the run wrote.

    `unmodelled` is `mark`, `miss` or `hit`. Unless it is `hit`, the sidecar
    `unmodelled_path(lcov)` is read when it exists, and an `lcov` report gets one next to its
    target, `unmodelled_path(target)`, with the lines of the kept files. `mark` keeps the counts
    and marks those lines in the `term` and built-in `html` reports, which then write `html`
    even when genhtml is installed and a kept line is marked. `miss` counts those lines, their
    functions and their branches as not hit in every report, and rewrites an `lcov` report.
    `hit` reads no sidecar.

    Raises:
        ValueError: `lcov` is not a tracefile, `unmodelled` is not a mode, the sidecar is not
            one, or an `lcov`, `xml` or `html` spec has no target.
        OSError: A target cannot be written.
    """
    check_mode(unmodelled)
    data = lcov.read_bytes()
    report = parse_lcov(data.decode())
    filtered = bool(include or omit)
    if filtered:
        report = report.filtered(include, omit, root)
    uses: UnmodelledUses | None = None
    unplaced = 0
    sidecar = unmodelled_path(lcov)
    if unmodelled != "hit" and sidecar.is_file():
        try:
            uses, unplaced = parse_unmodelled(sidecar.read_text())
        except ValueError as error:
            raise ValueError(f"{sidecar}: {error}") from error
        report = report.attached(uses, unplaced)
        uses = report.unmodelled
        report = report.with_unmodelled(unmodelled)
    rewritten = filtered or (unmodelled == "miss" and uses is not None)
    files: list[ReportSpec] = []
    for spec in specs:
        if spec.kind != "term":
            if spec.target is None:
                raise ValueError(f"an {spec.kind} report needs a target path")
            files.append(spec)
    written: list[Path] = []
    problems: list[str] = []
    for spec in files:
        target = spec.target
        assert target is not None
        if spec.kind == "html":
            problem = _write_html(report, lcov, target, rewritten, root)
            if problem is not None:
                problems.append(problem)
                continue
        elif spec.kind == "xml":
            stamp = int(time.time() * 1000) if timestamp is None else timestamp
            xml = render_cobertura(report, root=root, timestamp=stamp)
            _write_atomic(target, xml.encode())
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(report.to_lcov().encode() if rewritten else data)
            if uses is not None:
                unmodelled_path(target).write_text(dump_unmodelled(uses, unplaced))
            else:
                unmodelled_path(target).unlink(missing_ok=True)
        written.append(target)
    term = render_term(report, root=root) if any(spec.kind == "term" for spec in specs) else None
    return ReportResult(report.totals(), tuple(written), term, tuple(problems))
