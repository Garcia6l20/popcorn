import click

from popcorn import ConfigError
from popcorn.stm32._explain import explain


@click.group()
def part() -> None:
    """Inspect part descriptions."""


@part.command(name="explain")
@click.argument("name", metavar="PART")
@click.option("--trustzone", is_flag=True, help="List the TrustZone models of the part.")
def explain_command(name: str, trustzone: bool) -> None:
    """Print the model, row and source of each peripheral instance of PART."""
    try:
        explain(name, trustzone)
    except ConfigError as exc:
        raise click.ClickException(str(exc)) from exc
