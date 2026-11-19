import dataclasses
import json
import sys
from collections.abc import Callable, Collection, Sequence
from pathlib import Path

import click

from popcorn.cli._cpuprofile import to_cpuprofile
from popcorn.cli._files import expand, load_report
from popcorn.profiling import Report, ReportDiff, diff

TABLES = (
    "summary",
    "functions",
    "frames",
    "sleep",
    "wakes",
    "exceptions",
    "registers",
    "loops",
    "watchdog",
    "timeline",
)
SORT_FIELD = {
    "functions": "self",
    "frames": "self",
    "exceptions": "self_time_ps",
    "wakes": "time_ps",
    "loops": "time_ps",
    "sleep": "time_ps",
    "registers": "reads",
}


@click.group()
def profile() -> None:
    """Read the reports `--popcorn-profile` writes."""


@profile.command()
@click.argument("paths", nargs=-1, required=True, type=click.Path(path_type=Path))
def show(paths: tuple[Path, ...]) -> None:
    """Print the text of each report, a file or a directory of `*.json` reports."""
    files: list[Path] = []
    failed = False
    for path in paths:
        try:
            files.extend(expand([path]))
        except (FileNotFoundError, ValueError) as exc:
            click.echo(f"Error: {exc}", err=True)
            failed = True
    labelled = len(files) > 1 or len(paths) > 1
    printed = False
    for file in files:
        try:
            text = load_report(file).text()
        except (OSError, ValueError) as exc:
            click.echo(f"Error: {exc}", err=True)
            failed = True
            continue
        if printed:
            click.echo()
        if labelled:
            click.echo(f"== {file} ==")
        click.echo(text)
        printed = True
    if failed:
        click.get_current_context().exit(1)


@profile.command()
@click.argument("report", type=click.Path(path_type=Path, exists=True, dir_okay=False))
@click.option(
    "-o",
    "--output",
    type=click.Path(dir_okay=False, allow_dash=True),
    help="Write the folded stacks to this file, `-` or no option for stdout.",
)
def folded(report: Path, output: str | None) -> None:
    """Write the call tree of REPORT as folded stacks, for flamegraph.pl and speedscope."""
    try:
        loaded = load_report(report)
        if output is None or output == "-":
            loaded.write_folded(sys.stdout)
        else:
            count = loaded.write_folded(output)
            click.echo(f"{count} lines", err=True)
    except BrokenPipeError:
        raise
    except (OSError, ValueError) as exc:
        raise click.ClickException(str(exc)) from exc


@profile.command()
@click.argument("report", type=click.Path(path_type=Path, exists=True, dir_okay=False))
@click.option(
    "-o",
    "--output",
    required=True,
    type=click.Path(path_type=Path, dir_okay=False),
    help="The `.cpuprofile` file to write.",
)
def chrome(report: Path, output: Path) -> None:
    """Write the call tree of REPORT as a V8 `.cpuprofile` for Chrome DevTools and speedscope.

    A node is a call tree frame and its hitCount is the frame's self instructions. One time unit
    is one instruction. The time axis lays the call tree weights end to end, it is no recording
    of when they ran, and sleeps are absent. `popcorn.trace.to_chrome` converts a `.pctr`
    into a real timeline.
    """
    try:
        loaded = load_report(report)
    except (OSError, ValueError) as exc:
        raise click.ClickException(str(exc)) from exc
    try:
        profile_data = to_cpuprofile(loaded)
    except ValueError as exc:
        raise click.ClickException(f"{report}: {exc}") from exc
    try:
        output.write_text(json.dumps(profile_data, separators=(",", ":")), encoding="utf-8")
    except OSError as exc:
        raise click.ClickException(str(exc)) from exc


def _magnitude(value: int | float | None) -> int | float:
    return 0 if value is None else abs(value)


def _signed(value: int | float | None) -> str:
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:+,.4f}"
    return f"{value:+,}"


def _row_order(
    field: str, rows: dict[str, dict[str, int | float | None]]
) -> Callable[[str], tuple[int, int | float, str]]:
    def order(key: str) -> tuple[int, int | float, str]:
        row = rows[key]
        if field in row:
            return (0, -_magnitude(row[field]), key)
        return (1, -max(map(_magnitude, row.values()), default=0), key)

    return order


def render_diff(diff_: ReportDiff, top: int, tables: Sequence[str]) -> list[str]:
    """The text lines of `diff_` for the named `tables`, all when empty, at most `top` rows
    per table, all when 0."""
    lines: list[str] = []
    for name in TABLES:
        rows = diff_.get(name)
        if not rows or (tables and name not in tables):
            continue
        field = SORT_FIELD.get(name) or sorted(rows[sorted(rows)[0]])[0]
        keys = sorted(rows, key=_row_order(field, rows))
        shown = keys if top == 0 else keys[:top]
        lines.append(name)
        for key in shown:
            row = rows[key]
            fields = sorted(row, key=lambda item: (item != field, item))
            lines.append(f"  {key}  " + " ".join(f"{item}={_signed(row[item])}" for item in fields))
        if len(shown) < len(keys):
            lines.append(f"  ... {len(keys) - len(shown)} more")
    return lines


def _filtered(diff_: ReportDiff, tables: Sequence[str]) -> ReportDiff:
    return {name: rows for name, rows in diff_.items() if not tables or name in tables}


def _load(path: Path) -> Report:
    try:
        return load_report(path)
    except (OSError, ValueError) as exc:
        raise click.ClickException(str(exc)) from exc


def _json_text(data: object) -> str:
    return json.dumps(data, indent=2, sort_keys=True)


@profile.command(name="diff")
@click.argument("a", type=click.Path(path_type=Path, exists=True))
@click.argument("b", type=click.Path(path_type=Path, exists=True))
@click.option(
    "--top",
    default=10,
    show_default=True,
    type=click.IntRange(min=0),
    help="Rows printed per table, 0 prints all.",
)
@click.option(
    "--table",
    "tables",
    multiple=True,
    type=click.Choice(TABLES),
    help="Keep only this table in the output, repeatable.",
)
@click.option(
    "--json", "as_json", is_flag=True, help="Print the diff as JSON, `--top` does not apply."
)
def diff_command(a: Path, b: Path, top: int, tables: tuple[str, ...], as_json: bool) -> None:
    """Print what changed from report A to report B, or from directory A to directory B.

    Two directories are matched by file name. Each value is B less A, a value
    missing on one side counts as 0 and `-` marks a value B lost. Rows sort by the absolute
    change of one column per table: `self` for functions and frames, `self_time_ps` for
    exceptions, `time_ps` for wakes, loops and sleep, `reads` for registers, else the
    first column by name. A row without that column goes last. Exits 0 whether or not
    the reports differ.
    """
    if a.is_dir() != b.is_dir():
        raise click.UsageError("A and B must both be files or both be directories")
    if not a.is_dir():
        result = diff(_load(a), _load(b))
        if as_json:
            click.echo(_json_text({"a": str(a), "b": str(b), "diff": _filtered(result, tables)}))
            return
        lines = render_diff(_filtered(result, tables), top, tables)
        none = "no differences in the selected tables" if tables else "no differences"
        click.echo("\n".join(lines) if lines else none)
        return
    try:
        left = {path.name: path for path in expand([a])}
        right = {path.name: path for path in expand([b])}
    except (OSError, ValueError) as exc:
        raise click.ClickException(str(exc)) from exc
    only_a = sorted(left.keys() - right.keys())
    only_b = sorted(right.keys() - left.keys())
    pairs = {
        name: diff(_load(left[name]), _load(right[name])) for name in sorted(left & right.keys())
    }
    if as_json:
        kept = {name: _filtered(result, tables) for name, result in pairs.items()}
        click.echo(_json_text({"only_a": only_a, "only_b": only_b, "pairs": kept}))
        return
    if only_a:
        click.echo(f"only in A ({len(only_a)}): {', '.join(only_a)}")
    if only_b:
        click.echo(f"only in B ({len(only_b)}): {', '.join(only_b)}")
    unchanged = 0
    for name, result in pairs.items():
        lines = render_diff(_filtered(result, tables), top, tables)
        if not lines:
            unchanged += 1
            continue
        click.echo(f"== {name} ==")
        click.echo("\n".join(lines))
    click.echo(f"{unchanged} reports unchanged")


SUMMARY_SORT = ("name", "executed", "ms", "sleep", "irq")


@dataclasses.dataclass(frozen=True)
class _Row:
    name: str
    executed: int
    elapsed_ps: int
    sleep_share: float | None
    irq_share: float | None
    top: str | None


def _summary_row(path: Path, report: Report) -> _Row:
    irq_share = None
    if report.irq is not None:
        spent = sum(row.self_time_ps for row in report.irq.exceptions)
        irq_share = spent / report.elapsed_ps if report.elapsed_ps else 0.0
    functions = report.hotspots.functions if report.hotspots is not None else ()
    return _Row(
        path.stem,
        report.executed,
        report.elapsed_ps,
        None if report.sleep is None else report.sleep.share,
        irq_share,
        functions[0].name if functions else None,
    )


def _sort_value(row: _Row, key: str) -> int | float | None:
    if key == "executed":
        return row.executed
    if key == "ms":
        return row.elapsed_ps
    if key == "sleep":
        return row.sleep_share
    return row.irq_share


def _sorted_rows(rows: Sequence[_Row], key: str) -> list[_Row]:
    if key == "name":
        return sorted(rows, key=lambda row: row.name)

    def order(row: _Row) -> tuple[int, int | float, str]:
        number = _sort_value(row, key)
        return (1, 0, row.name) if number is None else (0, -number, row.name)

    return sorted(rows, key=order)


def _aggregate_functions(reports: Sequence[Report], count: int) -> list[tuple[str, int, int]]:
    """The `count` functions with the most self instructions summed by name over `reports`,
    as (name, self, reports holding it), ties by name."""
    totals: dict[str, list[int]] = {}
    for report in reports:
        if report.hotspots is None:
            continue
        seen: dict[str, int] = {}
        for row in report.hotspots.functions:
            seen[row.name] = seen.get(row.name, 0) + row.self
        for name, own in seen.items():
            held = totals.setdefault(name, [0, 0])
            held[0] += own
            held[1] += 1
    ranked = sorted(totals.items(), key=lambda item: (-item[1][0], item[0]))
    return [(name, held[0], held[1]) for name, held in ranked[:count]]


def _percent(share: float | None) -> str:
    return "-" if share is None else f"{100 * share:.1f}%"


def _table(
    header: Sequence[str], body: Sequence[Sequence[str]], left: Collection[int]
) -> list[str]:
    widths = [max(len(line[i]) for line in [header, *body]) for i in range(len(header))]
    return [
        "  ".join(
            cell.ljust(width) if i in left else cell.rjust(width)
            for i, (cell, width) in enumerate(zip(line, widths, strict=True))
        ).rstrip()
        for line in [header, *body]
    ]


@profile.command()
@click.argument("directory", type=click.Path(path_type=Path, exists=True, file_okay=False))
@click.option(
    "--sort",
    "sort_key",
    default="name",
    show_default=True,
    type=click.Choice(SUMMARY_SORT),
    help="Order of the rows. `name` ascending, the others descending with `-` rows last.",
)
@click.option(
    "--functions",
    default=0,
    show_default=True,
    type=click.IntRange(min=0),
    help="Also list this many functions with the most self instructions over all reports.",
)
@click.option("--json", "as_json", is_flag=True, help="Print the table as JSON.")
def summary(directory: Path, sort_key: str, functions: int, as_json: bool) -> None:
    """One row per report of DIRECTORY: instructions, time, sleep and exception shares, top.

    `ms` is the profile time, `sleep` the share of it asleep, `irq` the share spent in exception
    handlers, `top` the function with the most self instructions. `-` marks a section the report
    does not hold. A report that cannot be read is named on stderr, skipped, and the exit
    status is 1.
    """
    try:
        files = expand([directory])
    except (OSError, ValueError) as exc:
        raise click.ClickException(str(exc)) from exc
    loaded: list[tuple[Path, Report]] = []
    failed = False
    for file in files:
        try:
            loaded.append((file, load_report(file)))
        except (OSError, ValueError) as exc:
            click.echo(f"Error: {exc}", err=True)
            failed = True
    rows = _sorted_rows([_summary_row(file, report) for file, report in loaded], sort_key)
    total = sum(row.executed for row in rows)
    aggregated = _aggregate_functions([report for _, report in loaded], functions)
    if as_json:
        click.echo(
            _json_text(
                {
                    "reports": [
                        {
                            "name": row.name,
                            "executed": row.executed,
                            "elapsed_ps": row.elapsed_ps,
                            "sleep_share": row.sleep_share,
                            "irq_share": row.irq_share,
                            "top": row.top,
                        }
                        for row in rows
                    ],
                    "functions": [
                        {
                            "function": name,
                            "self": own,
                            "share": own / total if total else 0.0,
                            "reports": held,
                        }
                        for name, own, held in aggregated
                    ],
                }
            )
        )
    else:
        body = [
            [
                row.name,
                f"{row.executed:,}",
                f"{row.elapsed_ps / 1e9:.3f}",
                _percent(row.sleep_share),
                _percent(row.irq_share),
                row.top or "-",
            ]
            for row in rows
        ]
        for line in _table(["name", "executed", "ms", "sleep", "irq", "top"], body, (0, 5)):
            click.echo(line)
        if aggregated:
            click.echo()
            rows_ = [
                [name, f"{own:,}", _percent(own / total if total else 0.0), str(held)]
                for name, own, held in aggregated
            ]
            for line in _table(["function", "self", "share", "reports"], rows_, (0,)):
                click.echo(line)
        click.echo(f"{len(rows)} reports, {total:,} instructions")
    if failed:
        click.get_current_context().exit(1)
