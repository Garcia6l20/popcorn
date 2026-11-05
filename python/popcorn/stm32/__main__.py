"""CLI: python -m popcorn.stm32 explain <part> [--trustzone]."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence

from popcorn import ConfigError
from popcorn.stm32 import mcu, support

_HEADER = ("instance", "ip", "version", "model", "row", "source")


def _cmd_explain(args: argparse.Namespace) -> int:
    try:
        found = support(mcu(args.part), trustzone=args.trustzone)
    except ConfigError as exc:
        print(exc, file=sys.stderr)
        return 1
    rows = [
        (s.name, s.ip, s.ip_version or "-", s.model or "-", s.row or "-", s.source or "-")
        for s in found
    ]
    widths = [max(len(cell) for cell in column) for column in zip(_HEADER, *rows, strict=True)]
    for line in (_HEADER, *rows):
        print(
            "  ".join(cell.ljust(width) for cell, width in zip(line, widths, strict=True)).rstrip()
        )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m popcorn.stm32")
    subparsers = parser.add_subparsers(dest="command", required=True)
    explain = subparsers.add_parser(
        "explain", help="the model, row and source of each peripheral instance of a part"
    )
    explain.add_argument("part")
    explain.add_argument("--trustzone", action="store_true")
    explain.set_defaults(func=_cmd_explain)
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main())
