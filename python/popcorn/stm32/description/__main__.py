"""CLI: python -m popcorn.stm32.description build|sources <part> [--core CORE] [--out FILE]."""

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

from popcorn.stm32.description._cache import cached_description
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._sources import find_sources


def _cmd_build(args: argparse.Namespace) -> int:
    try:
        path = cached_description(args.part, core=args.core)
    except DescriptionError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    if args.out is not None:
        Path(args.out).write_text(path.read_text(encoding="utf-8"), encoding="utf-8")
        print(args.out)
    else:
        print(path)
    return 0


def _cmd_sources(args: argparse.Namespace) -> int:
    try:
        sources = find_sources(args.part, core=args.core)
    except DescriptionError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(f"part: {sources.part}")
    print(f"svd: {sources.svd}")
    print(f"mcu_db: {sources.mcu_db}")
    print(f"rzone: {sources.rzone}")
    print(f"cmsis_header: {sources.cmsis_header}")
    for patch in sources.patches:
        print(f"patch: {patch}")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m popcorn.stm32.description")
    subparsers = parser.add_subparsers(dest="command", required=True)

    build = subparsers.add_parser("build")
    build.add_argument("part")
    build.add_argument("--core")
    build.add_argument("--out")
    build.set_defaults(func=_cmd_build)

    sources_cmd = subparsers.add_parser("sources")
    sources_cmd.add_argument("part")
    sources_cmd.add_argument("--core")
    sources_cmd.set_defaults(func=_cmd_sources)

    args = parser.parse_args(argv)
    result: int = args.func(args)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
