from pathlib import Path

import click

from popcorn.coverage import Coverage, CoverageFileError, merge

INPUT = click.Path(path_type=Path, exists=True, dir_okay=False)
OUTPUT = click.Path(path_type=Path, dir_okay=False)


@click.group()
def coverage() -> None:
    """Merge and convert `.pcov` coverage files."""


@coverage.command(name="merge")
@click.argument("pcovs", nargs=-1, required=True, type=INPUT)
@click.option("-o", "--output", required=True, type=OUTPUT, help="The `.pcov` file to write.")
def merge_command(pcovs: tuple[Path, ...], output: Path) -> None:
    """Sum the PCOV files into one `.pcov` file."""
    try:
        merge(pcovs).save(output)
    except (OSError, CoverageFileError) as exc:
        raise click.ClickException(str(exc)) from exc


@coverage.command()
@click.argument("pcov", type=INPUT)
@click.option("--lcov", type=OUTPUT, help="Write an lcov tracefile.")
@click.option("--drcov", type=OUTPUT, help="Write a drcov file for Lighthouse.")
@click.option("--unmodelled", type=OUTPUT, help="Write the unmodelled-access JSON sidecar.")
@click.option("--no-branches", is_flag=True, help="Leave the branch records out of the lcov file.")
@click.option("--test-name", default="", help="The `TN:` record of the lcov file.")
def export(
    pcov: Path,
    lcov: Path | None,
    drcov: Path | None,
    unmodelled: Path | None,
    no_branches: bool,
    test_name: str,
) -> None:
    """Convert PCOV to lcov, drcov and the unmodelled-access sidecar.

    At least one of `--lcov`, `--drcov` and `--unmodelled` is needed.
    """
    if lcov is None and drcov is None and unmodelled is None:
        raise click.UsageError("give at least one of --lcov, --drcov and --unmodelled")
    try:
        loaded = Coverage.load(pcov)
        if lcov is not None:
            loaded.export_lcov(lcov, branches=not no_branches, test_name=test_name)
        if drcov is not None:
            dropped = loaded.export_drcov(drcov)
            if dropped:
                noun = "block" if dropped == 1 else "blocks"
                verb = "was" if dropped == 1 else "were"
                click.echo(
                    f"{drcov}: {dropped} {noun} outside every image {verb} left out", err=True
                )
        if unmodelled is not None:
            loaded.export_unmodelled(unmodelled)
    except (OSError, CoverageFileError) as exc:
        raise click.ClickException(str(exc)) from exc
