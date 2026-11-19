import json
import sys
from pathlib import Path

import click

from popcorn.cli._cpuprofile import to_cpuprofile
from popcorn.cli._files import expand, load_report


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
