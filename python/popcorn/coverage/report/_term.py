"""The terminal table of a coverage report."""

from __future__ import annotations

from pathlib import Path

from popcorn.coverage.report._model import Counts, LcovFile, LcovReport, Totals, display_name

HEADER = ("Name", "Lines", "Hit", "Cover", "Funcs", "Cover", "Branches", "Cover")
UNMODELLED = "Unmodelled"
EMPTY = "no firmware coverage"


def _percent(counts: Counts) -> str:
    if counts.found == 0:
        return "-"
    tenths = max(counts.hit * 1000 // counts.found, 1 if counts.hit else 0)
    return f"{tenths // 10}.{tenths % 10}%"


def _row(name: str, totals: Totals, unmodelled: int | None) -> tuple[str, ...]:
    row = (
        name,
        str(totals.lines.found),
        str(totals.lines.hit),
        _percent(totals.lines),
        str(totals.functions.found),
        _percent(totals.functions),
        str(totals.branches.found),
        _percent(totals.branches),
    )
    return row if unmodelled is None else (*row, str(unmodelled))


def _marked_lines(report: LcovReport) -> dict[str, list[int]]:
    by_file: dict[str, list[int]] = {}
    for path, line in report.unmodelled:
        by_file.setdefault(path, []).append(line)
    return by_file


def _marked(lines: list[int], file: LcovFile) -> int:
    return sum(1 for line in lines if line in file.lines)


def render_term(report: LcovReport, *, root: Path | None) -> str:
    """The table of `report`: one row per file sorted by display name, then a `TOTAL` row.

    A display name is the path relative to `root` for an absolute path under it, else the path as
    written. A cover column shows `-` where nothing was found. A percent is cut to one decimal
    and never rounded up, with 0.1% for any nonzero hit, so `100.0%` means every item was hit
    and `0.0%` means none. A report with no file renders as `no firmware coverage`.

    A report that holds lines which accessed unmodelled registers, `LcovReport.unmodelled` in
    mode `mark`, gets a last column `Unmodelled` with the count of those lines per file and in
    `TOTAL`. A report with `LcovReport.unplaced` hits adds a last line with that count, for the
    accesses to unmodelled registers whose instruction has no source line, or a line outside the
    files and functions the report keeps.
    """
    if not report.files:
        return EMPTY
    marks = bool(report.unmodelled)
    marked_lines = _marked_lines(report)
    header = (*HEADER, UNMODELLED) if marks else HEADER
    rows = sorted(
        (
            _row(
                file.path if root is None else display_name(file.path, root),
                file.totals(),
                _marked(marked_lines.get(file.path, []), file) if marks else None,
            )
            for file in report.files
        ),
        key=lambda row: row[0],
    )
    marked = sum(int(row[-1]) for row in rows) if marks else None
    rows.append(_row("TOTAL", report.totals(), marked))
    table = [header, *rows]
    widths = [max(len(row[column]) for row in table) for column in range(len(header))]

    def line(row: tuple[str, ...]) -> str:
        cells = [row[0].ljust(widths[0]), *(row[i].rjust(widths[i]) for i in range(1, len(row)))]
        return "  ".join(cells)

    rule = "-" * len(line(header))
    out = [line(header), rule, *map(line, rows[:-1]), rule, line(rows[-1])]
    if report.unplaced:
        out.append(
            f"{report.unplaced} unmodelled accesses came from code with no line in this report"
        )
    return "\n".join(out)
