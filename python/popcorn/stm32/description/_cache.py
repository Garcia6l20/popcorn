"""JSON cache of built PartDescriptions, keyed by part, core and the resolved sources' digests."""

import hashlib
import json
import os
from dataclasses import fields
from pathlib import Path

from popcorn.stm32.description._binder import check_bounds, dump_json
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._model import PartDescription
from popcorn.stm32.description._sources import (
    compute_source_digests,
    find_sources,
    generator_string,
)

_SCHEMA_VERSION = next(f.default for f in fields(PartDescription) if f.name == "schema_version")


def default_cache_dir() -> Path:
    """Description cache directory, `$XDG_CACHE_HOME/popcorn/mcu` or `~/.cache/popcorn/mcu`."""
    xdg = os.environ.get("XDG_CACHE_HOME")
    base = Path(xdg).expanduser() if xdg else Path("~/.cache").expanduser()
    return base / "popcorn" / "mcu"


def cache_key(part: str, core: str | None, generator: str, digests: dict[str, str]) -> str:
    payload = {
        "part": part,
        "core": core,
        "schema_version": _SCHEMA_VERSION,
        "generator": generator,
        "sources": digests,
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode()).hexdigest()


def cached_description(
    part: str,
    *,
    core: str | None = None,
    cache_dir: Path | None = None,
    svd: Path | None = None,
    cubemx_db: Path | None = None,
    cmsis_header: Path | None = None,
) -> Path:
    """Path of the cached JSON description of @p part, built on a miss."""
    sources = find_sources(part, core=core, svd=svd, cubemx_db=cubemx_db, cmsis_header=cmsis_header)
    digests = compute_source_digests(sources)
    generator = generator_string()
    key = cache_key(sources.part, core, generator, digests)

    cache_root = cache_dir or default_cache_dir()
    suffix = f"-{core}" if core else ""
    path = cache_root / f"{sources.part}{suffix}-{key[:16]}.json"
    if path.is_file():
        return path

    description = build_description(sources)
    check_bounds(description, file=str(path))
    text = dump_json(description)

    cache_root.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    tmp_path.write_text(text, encoding="utf-8")
    os.replace(tmp_path, path)
    return path
