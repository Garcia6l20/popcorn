"""CLI: python -m popcorn.stm32 explain <part> [--trustzone]."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence

from popcorn import ConfigError
from popcorn.stm32._explain import explain


def _cmd_explain(args: argparse.Namespace) -> int:
    try:
        explain(args.part, args.trustzone)
    except ConfigError as exc:
        print(exc, file=sys.stderr)
        return 1
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
