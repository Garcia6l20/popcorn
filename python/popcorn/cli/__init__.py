"""Command line interface of popcorn, installed as the `popcorn` console script."""

import os
import sys
from collections.abc import Sequence

import click

__all__ = ["cli", "main"]


@click.group(name="popcorn", context_settings={"help_option_names": ["-h", "--help"]})
@click.version_option(package_name="popcorn", prog_name="popcorn")
def cli() -> None:
    """Inspect popcorn profiling reports, coverage data and part descriptions."""


def main(argv: Sequence[str] | None = None) -> int:
    """Entry point of the `popcorn` console script. Returns the exit status.

    Always returns and never raises `SystemExit`. Click reports `Exit` codes as the return
    value, so a handler returning an `int` other than a `bool` sets the status. Handlers return
    `None`, and any other return value counts as success.
    """
    try:
        status = cli.main(
            args=list(argv) if argv is not None else None,
            prog_name="popcorn",
            standalone_mode=False,
        )
    except click.ClickException as exc:
        exc.show()
        return exc.exit_code
    except click.exceptions.Abort:
        click.echo("Aborted!", err=True)
        return 1
    except SystemExit as exc:
        return exc.code if isinstance(exc.code, int) else 1
    except BrokenPipeError:
        sys.stdout = open(os.devnull, "w")  # noqa: SIM115
        return 1
    if isinstance(status, int) and not isinstance(status, bool):
        return status
    return 0
