import sys
from typing import TextIO

from popcorn import _core
from popcorn.stm32 import mcu, support

_HEADER = ("instance", "ip", "version", "model", "row", "source", "factory", "variant", "reads")


def explain(part: str, trustzone: bool, out: TextIO | None = None) -> None:
    """Print the model, row and source of each peripheral instance of `part`.

    Args:
        part: The part name.
        trustzone: List the TrustZone models.
        out: The stream to print to, `sys.stdout` when `None`.

    Raises:
        ConfigError: `part` is unknown or no row serves it. Nothing is printed.
    """
    description = mcu(part)
    found = support(description, trustzone=trustzone)
    stream = sys.stdout if out is None else out
    resolved = {r.instance: r for r in _core.resolve_rows(description)}
    rows = [
        (
            s.name,
            s.ip,
            s.ip_version or "-",
            s.model or "-",
            s.row or "-",
            s.source or "-",
            resolved[s.name].factory or "-",
            resolved[s.name].variant or "-",
            resolved[s.name].reads or "-",
        )
        for s in found
    ]
    widths = [max(len(cell) for cell in column) for column in zip(_HEADER, *rows, strict=True)]
    for line in (_HEADER, *rows):
        print(
            "  ".join(cell.ljust(width) for cell, width in zip(line, widths, strict=True)).rstrip(),
            file=stream,
        )
