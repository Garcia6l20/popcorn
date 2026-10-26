"""The terminal table of a coverage report."""

from __future__ import annotations

from pathlib import Path

from popcorn.coverage.report._model import Counts, LcovReport, Totals, display_name

HEADER = ("Name", "Lines", "Hit", "Cover", "Funcs", "Cover", "Branches", "Cover")
EMPTY = "no firmware coverage"


def _percent(counts: Counts) -> str:
    if counts.found == 0:
        return "-"
    tenths = max(counts.hit * 1000 // counts.found, 1 if counts.hit else 0)
    return f"{tenths // 10}.{tenths % 10}%"


def _row(name: str, totals: Totals) -> tuple[str, ...]:
    return (
        name,
        str(totals.lines.found),
        str(totals.lines.hit),
        _percent(totals.lines),
        str(totals.functions.found),
        _percent(totals.functions),
        str(totals.branches.found),
        _percent(totals.branches),
    )


def render_term(report: LcovReport, *, root: Path | None) -> str:
    """The table of `report`: one row per file sorted by display name, then a `TOTAL` row.

    A display name is the path relative to `root` for an absolute path under it, else the path as
    written. A cover column shows `-` where nothing was found. A percent is cut to one decimal
    and never rounded up, with 0.1% for any nonzero hit, so `100.0%` means every item was hit
    and `0.0%` means none. A report with no file renders as `no firmware coverage`.
    """
    if not report.files:
        return EMPTY
    rows = sorted(
        (
            _row(file.path if root is None else display_name(file.path, root), file.totals())
            for file in report.files
        ),
        key=lambda row: row[0],
    )
    rows.append(_row("TOTAL", report.totals()))
    table = [HEADER, *rows]
    widths = [max(len(row[column]) for row in table) for column in range(len(HEADER))]

    def line(row: tuple[str, ...]) -> str:
        cells = [row[0].ljust(widths[0]), *(row[i].rjust(widths[i]) for i in range(1, len(row)))]
        return "  ".join(cells)

    rule = "-" * len(line(HEADER))
    return "\n".join([line(HEADER), rule, *map(line, rows[:-1]), rule, line(rows[-1])])
