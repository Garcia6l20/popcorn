"""CLI: python -m popcorn.stm32.description build|sources|golden|ship <part> [options]."""

import argparse
import os
import sys
from collections.abc import Sequence
from pathlib import Path

from popcorn.stm32.description._builder import build_description_with_report
from popcorn.stm32.description._cache import cached_description
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._golden import compute_golden, dump_tables, load_golden, write_golden
from popcorn.stm32.description._shipped import SHIPPED_DIR, build_shipped, shipped_text
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


def _golden_path(part: str) -> Path:
    return Path(__file__).parents[4] / "tests" / "data" / "stm32" / "golden" / f"{part}.json"


def _cmd_golden(args: argparse.Namespace) -> int:
    try:
        sources = find_sources(args.part, core=args.core)
        description, report = build_description_with_report(sources)
    except DescriptionError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    golden = compute_golden(description, report, svd_sha256=description.sources["svd"])
    path = _golden_path(sources.part)

    if args.dump is not None:
        dump_tables(Path(args.dump), description)
        print(args.dump)
        return 0

    if args.update:
        write_golden(path, golden)
        print(path)
        return 0

    committed = load_golden(path)
    if golden["counts"] != committed["counts"] or golden["tables"] != committed["tables"]:
        print(f"{path}: does not match the built description", file=sys.stderr)
        return 1
    print(path)
    return 0


def _cmd_ship(args: argparse.Namespace) -> int:
    if args.out is None and not args.check:
        print(
            "ship: --out is required to write, the package data directory of the source tree",
            file=sys.stderr,
        )
        return 1
    try:
        name, packed = build_shipped(args.part, pin_data=Path(args.pin_data), core=args.core)
    except DescriptionError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    out_dir = SHIPPED_DIR if args.out is None else Path(args.out)
    path = out_dir / f"{name}.json.xz"
    if args.check:
        from popcorn._part_description import description_digest, parse_description

        if not path.is_file():
            print(f"{path}: no shipped description", file=sys.stderr)
            return 1
        fresh = description_digest(
            parse_description(shipped_text(packed, file="fresh build"), "fresh build")
        )
        shipped = description_digest(
            parse_description(shipped_text(path.read_bytes(), file=str(path)), str(path))
        )
        if fresh != shipped:
            print(f"{path}: shipped {shipped}, fresh build {fresh}", file=sys.stderr)
            return 1
        print(path)
        return 0
    out_dir.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    try:
        tmp_path.write_bytes(packed)
        os.replace(tmp_path, path)
    except BaseException:
        tmp_path.unlink(missing_ok=True)
        raise
    print(path)
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

    golden = subparsers.add_parser("golden")
    golden.add_argument("part")
    golden.add_argument("--core")
    golden.add_argument("--update", action="store_true")
    golden.add_argument("--dump")
    golden.set_defaults(func=_cmd_golden)

    ship = subparsers.add_parser("ship")
    ship.add_argument("part")
    ship.add_argument("--core")
    ship.add_argument("--pin-data", required=True)
    ship.add_argument("--check", action="store_true")
    ship.add_argument("--out", help="directory to write the .json.xz into, required unless --check")
    ship.set_defaults(func=_cmd_ship)

    args = parser.parse_args(argv)
    result: int = args.func(args)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
