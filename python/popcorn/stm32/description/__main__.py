"""CLI: python -m popcorn.stm32.description build|sources <part> | golden|ship [<part> ...]
[options]."""

import argparse
import os
import sys
from collections.abc import Sequence
from pathlib import Path

from popcorn.stm32.description._builder import build_description_with_report
from popcorn.stm32.description._cache import cached_description
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._golden import compute_golden, dump_tables, load_golden, write_golden
from popcorn.stm32.description._layers import family_name
from popcorn.stm32.description._shipped import (
    SHIPPED_DIR,
    build_each,
    build_layers,
    compose_shipped,
    layer_families,
    match_name,
    shipped_names,
    write_layers,
)
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


GOLDEN_DIR = Path(__file__).parents[4] / "tests" / "data" / "stm32" / "golden"
"""Golden files, `<RefName>[-<core>].json`. A part ships exactly when its golden exists."""

DEFAULT_JOBS = min(10, os.cpu_count() or 1)


def golden_names() -> list[str]:
    """Shipped names with a golden file in GOLDEN_DIR, sorted."""
    return sorted(path.stem for path in GOLDEN_DIR.glob("*.json"))


def _golden_one(name: str, args: argparse.Namespace) -> int:
    part, _, core = name.partition("-")
    try:
        sources = find_sources(part, core=core or None)
        description, report = build_description_with_report(sources)
    except DescriptionError as exc:
        print(str(exc), file=sys.stderr)
        return 1

    golden = compute_golden(description, report, svd_sha256=description.sources["svd"])
    path = GOLDEN_DIR / f"{sources.part}{'-' + core if core else ''}.json"

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


def _cmd_golden(args: argparse.Namespace) -> int:
    if args.core and not args.parts:
        print("golden: --core needs a part list", file=sys.stderr)
        return 1
    if args.dump is not None and len(args.parts) != 1:
        print("golden: --dump needs exactly one part", file=sys.stderr)
        return 1
    names = [f"{part}-{args.core}" if args.core else part for part in args.parts]
    status = 0
    for name in names or golden_names():
        status = max(status, _golden_one(name, args))
    return status


def _named(args: argparse.Namespace, goldens: list[str]) -> list[str]:
    names: list[str] = []
    for part in args.parts:
        name = match_name(part, args.core, goldens, file=str(GOLDEN_DIR))
        if name is None:
            core = f" core {args.core}" if args.core else ""
            raise DescriptionError(
                str(GOLDEN_DIR), 0, part, f"a golden file of the part{core}, golden --update"
            )
        names.append(name)
    return names


def _check(names: list[str], out_dir: Path, pin_data: Path, jobs: int) -> int:
    from popcorn._part_description import description_digest, parse_description

    status = 0
    shipped = shipped_names(out_dir)
    present = [name for name in names if name in shipped]
    for name in names:
        if name not in shipped:
            print(f"{out_dir}: no shipped description of {name}", file=sys.stderr)
            status = 1
    for name, result in zip(
        present, build_each(present, pin_data=pin_data, jobs=jobs), strict=True
    ):
        try:
            if isinstance(result, DescriptionError):
                raise result
            shipped_text = compose_shipped(name, out_dir)
        except DescriptionError as exc:
            print(str(exc), file=sys.stderr)
            status = 1
            continue
        fresh_digest = description_digest(parse_description(result.text, "fresh build"))
        shipped_digest = description_digest(parse_description(shipped_text, name))
        if fresh_digest != shipped_digest:
            print(
                f"{out_dir}: {name} shipped {shipped_digest}, fresh build {fresh_digest}",
                file=sys.stderr,
            )
            status = 1
            continue
        print(f"{out_dir}: {name} {shipped_digest}")
    return status


def _cmd_ship(args: argparse.Namespace) -> int:
    if args.core and not args.parts:
        print("ship: --core needs a part list", file=sys.stderr)
        return 1
    if args.out is None and not args.check:
        print(
            "ship: --out is required to write, the package data directory of the source tree",
            file=sys.stderr,
        )
        return 1
    out_dir = SHIPPED_DIR if args.out is None else Path(args.out)
    pin_data = Path(args.pin_data)
    goldens = golden_names()
    try:
        named = _named(args, goldens)
        if args.check:
            return _check(named or goldens, out_dir, pin_data, args.jobs)
        if named:
            families = {family_name(name) for name in named}
        else:
            families = {family_name(name) for name in goldens} | layer_families(out_dir)
        names = [name for name in goldens if family_name(name) in families]
        files = build_layers(names, pin_data=pin_data, jobs=args.jobs)
    except DescriptionError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    removed = write_layers(out_dir, files, families)
    for file in sorted(files):
        print(out_dir / file)
    for file in removed:
        print(f"removed {out_dir / file}")
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

    golden = subparsers.add_parser("golden", help="no part: every golden")
    golden.add_argument("parts", nargs="*", metavar="part")
    golden.add_argument("--core")
    golden.add_argument("--update", action="store_true")
    golden.add_argument("--dump")
    golden.set_defaults(func=_cmd_golden)

    ship = subparsers.add_parser(
        "ship", help="rewrites every layer of the named parts' families, no part: every golden"
    )
    ship.add_argument("parts", nargs="*", metavar="part")
    ship.add_argument("--core")
    ship.add_argument("--pin-data", required=True)
    ship.add_argument("--check", action="store_true")
    ship.add_argument(
        "--out",
        help="layer directory, required to write, the imported package data for --check by default",
    )
    ship.add_argument("-j", "--jobs", type=int, default=DEFAULT_JOBS)
    ship.set_defaults(func=_cmd_ship)

    args = parser.parse_args(argv)
    result: int = args.func(args)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
