"""Descriptions shipped in the package as layers, and the lookup order of a part's description:
description directories, then the shipped layers, then a local build."""

import hashlib
import json
import lzma
import os
import re
import time
from pathlib import Path
from typing import Any, cast

from popcorn._errors import absent
from popcorn.stm32.description._binder import check_bounds, dump_json
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._cache import cached_description, default_cache_dir
from popcorn.stm32.description._errors import DescriptionError, MissingSource
from popcorn.stm32.description._layers import (
    LAYER_FORMAT,
    compose,
    dump,
    family_name,
    file_names,
    line_name,
)
from popcorn.stm32.description._sources import match_refname, pin_data_sources

SHIPPED_DIR = Path(__file__).parents[1] / "data"
"""Directory of the shipped layers, `<line>.parts.json.xz`, `<line>.line.json.xz` and
`<family>.blocks.json.xz`, described in `_layers`."""

MAX_SHIPPED_BYTES = 64 * 1024 * 1024
"""Largest decompressed layer file and largest composed description, 12 times the 5.1 MB of the
STM32U575QGIx description."""

STALE_SECONDS = 7 * 24 * 3600
"""Age after which a composed shipped description of other layer versions is removed."""

DESCRIPTION_DIRS_VARIABLE = "POPCORN_STM32_DESCRIPTION_DIRS"
BUILD_VARIABLE = "POPCORN_STM32_BUILD"

_PARTS_SUFFIX = ".parts.json.xz"


def _layer(data: bytes, *, file: str, kind: str) -> tuple[dict[str, Any], bytes]:
    raw = bounded_xz(data, MAX_SHIPPED_BYTES, file=file)
    try:
        value = json.loads(raw)
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise DescriptionError(file, 0, "<root>", "valid JSON") from exc
    if not isinstance(value, dict):
        raise DescriptionError(file, 0, "<root>", f"a {kind} layer object")
    layer = cast(dict[str, Any], value)
    if layer.get("format") != LAYER_FORMAT:
        raise DescriptionError(file, 0, "format", f"layer format {LAYER_FORMAT}")
    return layer, raw


def _names_in(path: Path) -> list[str]:
    layer, _ = _layer(path.read_bytes(), file=str(path), kind="parts")
    deltas = layer.get("parts")
    if not isinstance(deltas, dict):
        raise DescriptionError(str(path), 0, "parts", "an object")
    return list(cast(dict[str, Any], deltas))


def shipped_names(directory: Path | None = None) -> list[str]:
    """Names of the shipped parts, `<RefName>[-<core>]`, the keys of `parts` of every parts
    layer in @p directory, SHIPPED_DIR by default, sorted."""
    directory = SHIPPED_DIR if directory is None else directory
    return sorted(name for path in directory.glob(f"*{_PARTS_SUFFIX}") for name in _names_in(path))


def _layer_paths(name: str, directory: Path) -> tuple[Path, Path, Path]:
    parts, line, family = file_names(line_name(name), family_name(name))
    return directory / parts, directory / line, directory / family


def _read(paths: tuple[Path, Path, Path]) -> tuple[bytes, ...]:
    packed: list[bytes] = []
    for path in paths:
        try:
            packed.append(path.read_bytes())
        except FileNotFoundError as exc:
            raise DescriptionError(str(path), 0, "<root>", "a shipped layer file") from exc
    return tuple(packed)


def _compose_packed(name: str, paths: tuple[Path, Path, Path], packed: tuple[bytes, ...]) -> str:
    files = cast(tuple[str, str, str], tuple(str(path) for path in paths))
    parts, _ = _layer(packed[0], file=files[0], kind="parts")
    line, line_raw = _layer(packed[1], file=files[1], kind="line")
    family, _ = _layer(packed[2], file=files[2], kind="family")
    if parts.get("line_sha256") != hashlib.sha256(line_raw).hexdigest():
        raise DescriptionError(files[0], 0, "line_sha256", f"the SHA-256 of {files[1]}")
    return dump(compose(name, parts, line, family, limit=MAX_SHIPPED_BYTES, files=files))


def compose_shipped(name: str, directory: Path | None = None) -> str:
    """JSON text of the description of the shipped part @p name, `<RefName>[-<core>]`, its delta
    in its line's parts layer over its line layer over its family's blocks, in @p directory,
    SHIPPED_DIR by default. The text is what dump_json writes for the same description.

    DescriptionError naming the file for a layer that is not one complete xz stream of at most
    MAX_SHIPPED_BYTES, a layer of another format, a `line_sha256` that is not the SHA-256 of the
    decompressed line layer, and every refusal of `_layers.compose`: a missing or tampered
    block, an unknown instance, a composition above MAX_SHIPPED_BYTES counted before it is
    built, a missing layer file."""
    paths = _layer_paths(name, SHIPPED_DIR if directory is None else directory)
    return _compose_packed(name, paths, _read(paths))


def _match(part: str, core: str | None, stems: list[str], *, file: str) -> str | None:
    refnames: list[str] = []
    for stem in stems:
        refname, _, stem_core = stem.partition("-")
        if (stem_core or None) == (core or None):
            refnames.append(refname)
    found = match_refname(part, refnames, file=file)
    if found is None:
        return None
    return f"{found}-{core}" if core else found


def bounded_xz(data: bytes, limit: int, *, file: str) -> bytes:
    """The decompressed bytes of the xz stream @p data, DescriptionError naming @p file when it
    is not one complete xz stream or decompresses to more than @p limit bytes. Stops at
    @p limit, so a stream that expands without bound costs at most @p limit bytes of work."""
    decompressor = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    try:
        out = decompressor.decompress(data, max_length=limit)
    except lzma.LZMAError as exc:
        raise DescriptionError(file, 0, "<root>", "an xz stream") from exc
    if not decompressor.eof:
        if decompressor.needs_input:
            raise DescriptionError(file, 0, "<root>", "a complete xz stream")
        raise DescriptionError(file, 0, "<root>", f"at most {limit} bytes decompressed")
    if decompressor.unused_data:
        raise DescriptionError(file, 0, "<root>", "one xz stream and nothing after it")
    return out


def shipped_description(
    part: str, *, core: str | None = None, cache_dir: Path | None = None
) -> Path | None:
    """Path of the composed shipped description of @p part, None when none ships.

    Composed once into `<cache>/shipped/<name>-<sha16>.json`, sha16 the first 16 hex digits of
    the SHA-256 over the xz bytes of the part's parts, line and family layers, in that order. A
    change to any part of a line renames the cache file of every part of the line. The cache is
    shared by every popcorn of the user, so each layer version keeps its own file: a hit
    refreshes its mtime when the cache is writable, and a write removes the files of the same
    name and another version unused for STALE_SECONDS, except a file the directory's sticky bit
    keeps for its owner. Bound: one file per shipped name per layer version used in the last 7
    days."""
    name = _match(part, core, shipped_names(), file=str(SHIPPED_DIR))
    if name is None:
        return None
    paths = _layer_paths(name, SHIPPED_DIR)
    packed = _read(paths)
    hasher = hashlib.sha256()
    for data in packed:
        hasher.update(data)
    digest = hasher.hexdigest()[:16]
    directory = (cache_dir or default_cache_dir()) / "shipped"
    path = directory / f"{name}-{digest}.json"
    try:
        os.utime(path)
    except FileNotFoundError:
        pass
    except OSError:
        if path.is_file():
            return path
        raise
    else:
        return path
    data = _compose_packed(name, paths, packed).encode("ascii")
    directory.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    try:
        tmp_path.write_bytes(data)
        os.replace(tmp_path, path)
    except BaseException:
        tmp_path.unlink(missing_ok=True)
        raise
    other = re.compile(rf"{re.escape(name)}-[0-9a-f]{{16}}\.json")
    cutoff = time.time() - STALE_SECONDS
    for stale in directory.iterdir():
        if stale == path or not other.fullmatch(stale.name):
            continue
        try:
            if stale.stat().st_mtime < cutoff:
                stale.unlink(missing_ok=True)
        except FileNotFoundError, PermissionError:
            pass
    return path


def _env_dirs() -> list[Path]:
    value = os.environ.get(DESCRIPTION_DIRS_VARIABLE)
    if not value:
        return []
    return [Path(entry).expanduser() for entry in value.split(os.pathsep) if entry]


def _local_build_forced() -> bool:
    value = os.environ.get(BUILD_VARIABLE)
    if not value:
        return False
    if value == "local":
        return True
    raise DescriptionError(BUILD_VARIABLE, 0, BUILD_VARIABLE, f"unset or local, not {value!r}")


def description_path(
    part: str,
    *,
    core: str | None = None,
    svd: Path | None = None,
    cubemx_db: Path | None = None,
    cmsis_header: Path | None = None,
    cache_dir: Path | None = None,
) -> Path:
    """Path of the JSON description of @p part, the first of:

    1. `<RefName>[-<core>].json` in a directory of POPCORN_STM32_DESCRIPTION_DIRS, an os.pathsep
       list searched in order, the part matched as the db matches it.
    2. The shipped layers, unless @p svd, @p cubemx_db or @p cmsis_header is given or
       POPCORN_STM32_BUILD is `local`.
    3. A description built from ST's files and cached, cached_description.

    DescriptionError when POPCORN_STM32_BUILD is set to anything but `local`, checked first.
    MissingSource when no step finds one and ST's files are missing, naming the shipped parts
    unless step 2 was skipped."""
    local_only = _local_build_forced() or any(
        value is not None for value in (svd, cubemx_db, cmsis_header)
    )
    for directory in _env_dirs():
        if absent(directory):
            continue
        stems = sorted(path.stem for path in directory.glob("*.json"))
        name = _match(part, core, stems, file=str(directory))
        if name is not None:
            return directory / f"{name}.json"
    if not local_only:
        shipped = shipped_description(part, core=core, cache_dir=cache_dir)
        if shipped is not None:
            return shipped
    try:
        return cached_description(
            part,
            core=core,
            cache_dir=cache_dir,
            svd=svd,
            cubemx_db=cubemx_db,
            cmsis_header=cmsis_header,
        )
    except MissingSource as exc:
        if local_only:
            raise
        shipped_parts = ", ".join(shipped_names()) or "none"
        raise MissingSource(
            exc.file,
            exc.key,
            f"a shipped description, shipped: {shipped_parts}, or ST's files: {exc.expected}",
            path=exc.path,
        ) from exc


def build_shipped(part: str, *, pin_data: Path, core: str | None = None) -> tuple[str, bytes]:
    """Name, `<RefName>[-<core>]`, and xz bytes of the shipped description of @p part, built
    from pin_data_sources. Equal inputs give equal bytes. DescriptionError when no CMSIS device
    header is found: a description without its IRQ names would differ from a local build."""
    sources = pin_data_sources(part, pin_data=pin_data, core=core)
    if sources.cmsis_header is None:
        raise DescriptionError(
            sources.part, 0, "cmsis_header", "a CMSIS device header for the IRQ names"
        )
    name = f"{sources.part}-{core}" if core else sources.part
    description = build_description(sources)
    check_bounds(description, file=name)
    text = dump_json(description)
    packed = lzma.compress(
        text.encode("utf-8"), format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, preset=9
    )
    return name, packed
