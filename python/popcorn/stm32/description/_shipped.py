"""Descriptions shipped in the package as `.json.xz`, and the lookup order of a part's
description: description directories, then the shipped file, then a local build."""

import hashlib
import lzma
import os
import re
from pathlib import Path

from popcorn._errors import absent
from popcorn.stm32.description._cache import cached_description, default_cache_dir
from popcorn.stm32.description._errors import DescriptionError, MissingSource
from popcorn.stm32.description._sources import match_refname

SHIPPED_DIR = Path(__file__).parents[1] / "data"
"""Directory of the shipped `<RefName>[-<core>].json.xz` descriptions."""

MAX_SHIPPED_BYTES = 64 * 1024 * 1024
"""Largest decompressed shipped description, 12 times the 5.1 MB of the STM32U575QGIx one."""

DESCRIPTION_DIRS_VARIABLE = "POPCORN_STM32_DESCRIPTION_DIRS"
BUILD_VARIABLE = "POPCORN_STM32_BUILD"


def shipped_names() -> list[str]:
    """Stems of the shipped descriptions, `<RefName>[-<core>]`, sorted."""
    return sorted(path.name.removesuffix(".json.xz") for path in SHIPPED_DIR.glob("*.json.xz"))


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
    """Path of the decompressed shipped description of @p part, None when none ships.

    Decompressed once into `<cache>/shipped/<name>-<sha16>.json`, sha16 the first 16 hex digits
    of the SHA-256 of the `.json.xz`. Writing a new one removes the older files of the same
    name, so the cache holds at most one file per shipped description."""
    name = _match(part, core, shipped_names(), file=str(SHIPPED_DIR))
    if name is None:
        return None
    packed_path = SHIPPED_DIR / f"{name}.json.xz"
    packed = packed_path.read_bytes()
    digest = hashlib.sha256(packed).hexdigest()[:16]
    directory = (cache_dir or default_cache_dir()) / "shipped"
    path = directory / f"{name}-{digest}.json"
    if path.is_file():
        return path
    data = bounded_xz(packed, MAX_SHIPPED_BYTES, file=str(packed_path))
    directory.mkdir(parents=True, exist_ok=True)
    tmp_path = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    tmp_path.write_bytes(data)
    os.replace(tmp_path, path)
    older = re.compile(rf"{re.escape(name)}-[0-9a-f]{{16}}\.json")
    for stale in directory.iterdir():
        if stale != path and older.fullmatch(stale.name):
            stale.unlink(missing_ok=True)
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
    raise DescriptionError(BUILD_VARIABLE, 0, BUILD_VARIABLE, "unset or local")


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
    2. The shipped description, unless @p svd, @p cubemx_db or @p cmsis_header is given or
       POPCORN_STM32_BUILD is `local`.
    3. A description built from ST's files and cached, cached_description.

    DescriptionError when POPCORN_STM32_BUILD is set to anything but `local`, checked first.
    MissingSource when no step finds one and ST's files are missing, naming the shipped parts."""
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
        shipped_parts = ", ".join(shipped_names()) or "none"
        raise MissingSource(
            exc.file,
            exc.key,
            f"a shipped description, shipped: {shipped_parts}, or ST's files: {exc.expected}",
            path=exc.path,
        ) from exc
