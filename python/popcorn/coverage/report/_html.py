"""The built-in html report: an index and one annotated source page per file, no scripts."""

from __future__ import annotations

from html import escape
from pathlib import Path

from popcorn.coverage.report._genhtml import BUILTIN_MARKER
from popcorn.coverage.report._model import (
    Counts,
    LcovFile,
    LcovReport,
    display_name,
    resolve,
    truncated_percent,
)

SOURCE_LIMIT = 16 * 1024 * 1024

STYLE = """body { font-family: sans-serif; margin: 1.5em; }
table { border-collapse: collapse; }
th, td { padding: 0.15em 0.7em; text-align: right; }
th:first-child, td:first-child { text-align: left; }
table.src td { padding: 0 0.7em; font-family: monospace; white-space: pre; text-align: left; }
table.src td.no, table.src td.hits { text-align: right; color: #666; }
tr.hit td.src { background: #d8f0d8; }
tr.miss td.src { background: #f6d0d0; }
tr.partial td.src { background: #f8ecb8; }
.notice { color: #8a2a2a; }
"""


def _rate(counts: Counts) -> str:
    return f"{truncated_percent(counts)}% ({counts.hit}/{counts.found})"


def _page(title: str, body: str) -> str:
    name = escape(title)
    return (
        '<!DOCTYPE html>\n<html lang="en"><head><meta charset="utf-8">'
        f'<title>{name}</title><link rel="stylesheet" href="style.css"></head>'
        f"<body><h1>{name}</h1>\n{body}</body></html>\n"
    )


def _read_source(path: Path) -> list[str] | None:
    try:
        if not path.is_file() or path.stat().st_size > SOURCE_LIMIT:
            return None
        text = path.read_bytes().decode(errors="replace")
    except OSError:
        return None
    lines = text.split("\n")
    if lines[-1] == "":
        lines.pop()
    return [line.removesuffix("\r") for line in lines]


def _source_page(file: LcovFile, name: str, root: Path) -> str:
    source = _read_source(resolve(file.path, root))
    untaken = {branch.line for branch in file.branches if not branch.taken}
    parts = [f'<p><a href="index.html">index</a> {_rate(file.totals().lines)} lines</p>\n']
    if source is None:
        parts.append(
            '<p class="notice">The source file is not a regular file under '
            f"{SOURCE_LIMIT // (1 << 20)} MiB, or could not be read.</p>\n"
        )
    if file.functions:
        parts.append("<table><tr><th>function</th><th>line</th><th>hits</th></tr>\n")
        for function in sorted(file.functions, key=lambda item: (item.start, item.name)):
            parts.append(
                f"<tr><td>{escape(function.name)}</td><td>{function.start}</td>"
                f"<td>{function.hits}</td></tr>\n"
            )
        parts.append("</table>\n")
    if source is None:
        shown = sorted(file.lines)
    else:
        shown = [*range(1, len(source) + 1), *sorted(n for n in file.lines if n > len(source))]
    parts.append('<table class="src">\n')
    for number in shown:
        hits = file.lines.get(number)
        if hits is None:
            state = "none"
        elif hits == 0:
            state = "miss"
        elif number in untaken:
            state = "partial"
        else:
            state = "hit"
        text = ""
        if source is not None and number <= len(source):
            text = escape(source[number - 1])
        count = "" if hits is None else str(hits)
        parts.append(
            f'<tr class="{state}"><td class="no">{number}</td><td class="hits">{count}</td>'
            f'<td class="src">{text}</td></tr>\n'
        )
    parts.append("</table>\n")
    return _page(name, "".join(parts))


def render_html(report: LcovReport, out: Path, *, root: Path) -> tuple[Path, ...]:
    """Write the html report of `report` into the directory `out`, created when missing.

    `index.html` lists the totals and one row per file, sorted by display name, with line,
    function and branch rates. A file's page is `src-<n>.html` for its position `n` in that
    order, so no source path reaches a file name. The page holds the functions with their hits
    and every source line with its count, coloured hit, miss, or partial when a branch of a hit
    line was not taken. The source is read from `resolve(path, root)` and split on newlines only.
    A source that cannot be read, is not a regular file or exceeds `SOURCE_LIMIT` bytes gives
    the counted lines with no text and a notice. A page lists the source lines and the counted
    lines past its end, so its size is bounded by the source and the tracefile. Every source line
    and name is escaped, and the pages load only the local `style.css`. The empty file
    `BUILTIN_MARKER` marks the directory as a report this function wrote, so a later run may
    empty it.

    Returns:
        The report files written, `style.css` first and `index.html` second.
    """
    out.mkdir(parents=True, exist_ok=True)
    (out / BUILTIN_MARKER).write_bytes(b"")
    ordered = sorted(report.files, key=lambda file: (display_name(file.path, root), file.path))
    rows: list[str] = []
    written = [out / "style.css", out / "index.html"]
    written[0].write_text(STYLE)
    for number, file in enumerate(ordered):
        name = display_name(file.path, root)
        page = out / f"src-{number}.html"
        page.write_text(_source_page(file, name, root))
        written.append(page)
        totals = file.totals()
        rows.append(
            f'<tr><td><a href="src-{number}.html">{escape(name)}</a></td>'
            f"<td>{_rate(totals.lines)}</td><td>{_rate(totals.functions)}</td>"
            f"<td>{_rate(totals.branches)}</td></tr>\n"
        )
    total = report.totals()
    body = (
        "<table><tr><th>file</th><th>lines</th><th>functions</th><th>branches</th></tr>\n"
        f"<tr><td>TOTAL</td><td>{_rate(total.lines)}</td><td>{_rate(total.functions)}</td>"
        f"<td>{_rate(total.branches)}</td></tr>\n{''.join(rows)}</table>\n"
    )
    written[1].write_text(_page("firmware coverage", body))
    return tuple(written)
