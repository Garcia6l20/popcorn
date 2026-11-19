import json
import sys
from collections.abc import Callable, Sequence
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
