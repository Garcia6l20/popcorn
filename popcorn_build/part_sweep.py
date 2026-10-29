"""Sweep STM32 parts: build each description, attach a Machine at TZEN 0 and 1, write a JSON table.

Run from the checkout, `python -m popcorn_build.part_sweep --out <file> [--parts <glob> ...]
[-j <jobs>] [--source local|shipped]`. Each part runs in a child interpreter of its own, so a
crash or a hang costs that part's row only.
"""

import argparse
import fnmatch
import json
import os
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections.abc import Callable, Sequence
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any, Literal

Row = dict[str, Any]
Source = Literal["local", "shipped"]

FORMAT = 1
"""Version of the JSON table layout."""
ERROR_CHARS = 300
"""Characters of an exception's text a row keeps."""
CHILD_TIMEOUT_S = 120.0
"""Wall time one part's child gets before its row says `timeout`."""
LOCAL_SOURCES = ("POPCORN_STM32_DESCRIPTION_DIRS", "POPCORN_STM32_BUILD")
"""Variables that make `mcu()` read a description other than the shipped one."""

_ROOT = Path(__file__).resolve().parent.parent
CHILD = (sys.executable, "-m", "popcorn_build.part_sweep", "--child")
"""Command of the child that sweeps one part, the part's name appended."""
ROW_PREFIX = "PART_SWEEP_ROW "
"""Start of the stdout line that carries a child's JSON row."""


def _error(exc: BaseException) -> str:
    return f"{type(exc).__name__}: {str(exc)[:ERROR_CHARS]}"


def _db_dir() -> Path:
    from popcorn.stm32.description._sources import default_search_paths

    return default_search_paths().cubemx_db


def _part_facts(refname: str) -> Row:
    from popcorn.stm32.description._cubemx import bonded_pins, read_part

    facts: Row = {
        "line": None,
        "die": None,
        "package": None,
        "flash_kib": None,
        "ram_kib": None,
        "bonded": None,
        "db_error": None,
    }
    db_file = _db_dir() / "mcu" / f"{refname}.xml"
    if not db_file.is_file():
        return facts
    try:
        part = read_part(db_file.read_bytes(), file=str(db_file))
    except Exception as exc:
        facts["db_error"] = _error(exc)
        return facts
    facts.update(
        line=part.line,
        die=part.die,
        package=part.package,
        flash_kib=part.flash_kib[0],
        ram_kib=part.ram_kib[0],
        bonded={port: f"{mask:#06x}" for port, mask in bonded_pins(part).items()},
    )
    return facts


def sweep_part(name: str) -> Row:
    """The row of part @p name, `<RefName>[-<core>]`, from the description `mcu()` gives it in
    this environment. A name holding `(`, a CubeMX range stem such as `STM32C011F(4-6)Px`, is a
    RefName whole. Part facts come from the CubeMX db when it holds the part and its file
    reads, else they are None, with `db_error` set for a file that does not read. An exception
    in a stage ends the row there, its type and text in that stage's field."""
    import popcorn
    from popcorn.stm32 import mcu, support
    from popcorn.stm32.description._patches import is_smps_part

    refname, _, core_name = (name, "", "") if "(" in name else name.partition("-")
    core = core_name or None
    row: Row = {"part": name, "error": None, **_part_facts(refname), "smps": is_smps_part(refname)}
    row["attach"] = None
    row["instances"] = None
    try:
        description = mcu(refname, core=core)
    except Exception as exc:
        row["description"] = {"ok": False, "digest": None, "error": _error(exc)}
        return row
    row["description"] = {
        "ok": True,
        "digest": popcorn.description_digest(description),
        "error": None,
    }

    attach: Row = {}
    for tz in (False, True):
        option_bytes = popcorn.OptionBytes(tzen=True) if tz else None
        try:
            machine = popcorn.MachineDescription(
                part=refname, core=core, images=[], option_bytes=option_bytes
            ).machine()
            del machine
            attach[f"tz{int(tz)}"] = "ok"
        except Exception as exc:
            attach[f"tz{int(tz)}"] = _error(exc)
    row["attach"] = attach

    peripherals = [p for p in description.peripherals if p.alias_of is None]
    instances: Row = {
        "total": len(peripherals),
        "modelled_tz0": None,
        "modelled_tz1": None,
        "no_ip_version": sorted(p.name for p in peripherals if not p.ip_version),
    }
    for tz in (False, True):
        try:
            served = support(description, trustzone=tz)
            instances[f"modelled_tz{int(tz)}"] = sum(1 for s in served if s.model == "builtin")
        except Exception as exc:
            instances[f"modelled_tz{int(tz)}"] = _error(exc)
    row["instances"] = instances
    return row


def _run_child(name: str, *, child: Sequence[str], env: dict[str, str], timeout: float) -> Row:
    try:
        done = subprocess.run(
            [*child, name],
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=_ROOT,
            env=env,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return {"part": name, "error": "timeout"}
    lines = [line for line in done.stdout.splitlines() if line.startswith(ROW_PREFIX)]
    row: object = None
    if done.returncode == 0 and lines:
        try:
            row = json.loads(lines[-1].removeprefix(ROW_PREFIX))
        except json.JSONDecodeError:
            row = None
    if not isinstance(row, dict):
        return {
            "part": name,
            "error": "crashed",
            "returncode": done.returncode,
            "stderr": done.stderr[-ERROR_CHARS:],
        }
    return {**row, "part": name}


def child_env(source: Source) -> dict[str, str]:
    """Environment of a child: `mcu()` builds from ST's files for `local` and loads the shipped
    description for `shipped`, with no description directory in either."""
    env = {k: v for k, v in os.environ.items() if k not in LOCAL_SOURCES}
    if source == "local":
        env["POPCORN_STM32_BUILD"] = "local"
    return env


def sweep(
    parts: Sequence[str],
    *,
    source: Source = "local",
    jobs: int = 10,
    timeout: float = CHILD_TIMEOUT_S,
    child: Sequence[str] = CHILD,
    on_row: Callable[[Row], None] | None = None,
) -> list[Row]:
    """One row per name of @p parts, sorted by part, each from its own child process. At most
    @p jobs children run at once. A child that exits non-zero or prints no JSON object gives
    `{"part": ..., "error": "crashed"}` with its exit code and stderr tail, one past @p timeout
    seconds gives `"error": "timeout"`. A child prints its row as JSON on a stdout line that
    starts with `ROW_PREFIX`, the last such line counts. @p on_row sees each row as it completes."""
    if jobs < 1:
        raise ValueError(f"jobs must be at least 1, got {jobs}")
    env = child_env(source)

    def one(name: str) -> Row:
        row = _run_child(name, child=child, env=env, timeout=timeout)
        if on_row is not None:
            on_row(row)
        return row

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        return list(pool.map(one, sorted(set(parts))))


def db_parts(globs: Sequence[str]) -> list[str]:
    """RefNames of the CubeMX db's `mcu/STM32*.xml` part files matching any of @p globs, every
    part when @p globs is empty, sorted."""
    names = sorted(p.stem for p in (_db_dir() / "mcu").glob("STM32*.xml"))
    if not globs:
        return names
    return [n for n in names if any(fnmatch.fnmatchcase(n, g) for g in globs)]


def db_version() -> str | None:
    """Release of the CubeMX db, such as `6.0.181`, from its `package.xml`, None without one."""
    package = _db_dir() / "package.xml"
    if not package.is_file():
        return None
    found = ET.parse(package).getroot().find("PackDescription")
    release = found.get("Release") if found is not None else None
    return release.removeprefix("DB.") if release else None


def _describe() -> str | None:
    done = subprocess.run(
        ["git", "describe", "--always", "--dirty"],
        capture_output=True,
        text=True,
        cwd=_ROOT,
        check=False,
    )
    if done.returncode != 0:
        return None
    return done.stdout.strip() or None


def table(rows: list[Row]) -> Row:
    """The JSON document of @p rows: format, CubeMX db release, `git describe` of the checkout."""
    return {"format": FORMAT, "cubemx_db": db_version(), "popcorn": _describe(), "parts": rows}


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m popcorn_build.part_sweep")
    parser.add_argument("--out", type=Path, help="JSON file to write")
    parser.add_argument("--parts", nargs="*", default=[], help="RefName globs, default every part")
    parser.add_argument("-j", "--jobs", type=int, default=10, help="children at once")
    parser.add_argument("--source", choices=("local", "shipped"), default="local")
    parser.add_argument("--child", metavar="PART", help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.child is not None:
        print(ROW_PREFIX + json.dumps(sweep_part(args.child)), flush=True)
        return 0
    if args.out is None:
        parser.error("--out is required")
    source: Source = args.source
    if source == "shipped":
        from popcorn.stm32 import shipped_parts

        names = [
            n
            for n in shipped_parts()
            if not args.parts or any(fnmatch.fnmatchcase(n, g) for g in args.parts)
        ]
    else:
        mcu_dir = _db_dir() / "mcu"
        if not mcu_dir.is_dir():
            parser.error(f"no CubeMX db part directory at {mcu_dir}, set POPCORN_CUBEMX_DB")
        names = db_parts(args.parts)
    if not names:
        parser.error(f"no {source} part matches {args.parts or 'any glob'}")

    def progress(row: Row) -> None:
        print(row["part"], row["error"] or "done", file=sys.stderr, flush=True)

    rows = sweep(names, source=source, jobs=args.jobs, on_row=progress)
    args.out.write_text(json.dumps(table(rows), indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
