"""Source lookup: SVD, CubeMX db, rzone, GPIO modes, CMSIS header and patch files for a part."""

import difflib
import hashlib
import importlib.metadata
import os
import re
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from popcorn._errors import absent
from popcorn.stm32.description._cubemx import (
    DbPart,
    gpio_modes_file_name,
    read_part,
    rzone_file_name,
)
from popcorn.stm32.description._errors import DescriptionError, MissingSource
from popcorn.stm32.description._patches import load_patches

_PATCHES_DIR = Path(__file__).parent / "patches"
_RZONE_FACTS_DIR = Path(__file__).parent / "rzone"

_CORE_SUFFIX_RE = re.compile(r"_CM[0-9][A-Za-z0-9]*$")
_VERSION_RE = re.compile(r"(\d+(?:\.\d+)*)$")


@dataclass(slots=True, frozen=True, kw_only=True)
class SearchPaths:
    svd_dirs: list[Path]
    cubemx_db: Path
    cmsis_device_dirs: list[Path] | None


@dataclass(slots=True, frozen=True, kw_only=True)
class Sources:
    """Files a description is built from. The memory map and core values come from `rzone`
    when set, else from `rzone_facts`, and the source digests record only the one read. Neither
    set is a DescriptionError at build time."""

    part: str
    svd: Path
    mcu_db: Path
    rzone: Path | None
    cmsis_header: Path | None
    patches: list[Path]
    gpio_modes: Path | None = None
    rzone_facts: Path | None = None


def _version_key(path: Path) -> tuple[int, ...]:
    match = _VERSION_RE.search(path.name)
    if not match:
        return ()
    return tuple(int(part) for part in match.group(1).split("."))


def _env_paths(name: str) -> list[Path] | None:
    value = os.environ.get(name)
    if not value:
        return None
    return [Path(p) for p in value.split(os.pathsep) if p]


def default_svd_dirs(st_root: Path | None = None) -> list[Path]:
    dirs: list[Path] = []
    home_st = st_root if st_root is not None else Path("~/st").expanduser()
    for clt_dir in sorted(home_st.glob("stm32cubeclt_*"), key=_version_key, reverse=True):
        candidate = clt_dir / "STMicroelectronics_CMSIS_SVD"
        if candidate.is_dir():
            dirs.append(candidate)
    for ide_dir in sorted(home_st.glob("stm32cubeide_*"), key=_version_key, reverse=True):
        plugin_glob = "plugins/com.st.stm32cube.ide.mcu.productdb.debug_*"
        for plugin_dir in sorted(ide_dir.glob(plugin_glob)):
            candidate = plugin_dir / "resources" / "cmsis" / "STMicroelectronics_CMSIS_SVD"
            if candidate.is_dir():
                dirs.append(candidate)
    return dirs


def default_search_paths() -> SearchPaths:
    """SearchPaths from POPCORN_STM32_SVD_DIRS/POPCORN_CUBEMX_DB/POPCORN_CMSIS_DEVICE_DIRS when
    set (os.pathsep lists), else the spec 04 default locations under the user's home."""
    svd_dirs = _env_paths("POPCORN_STM32_SVD_DIRS")
    if svd_dirs is None:
        svd_dirs = default_svd_dirs()
    cubemx_db_list = _env_paths("POPCORN_CUBEMX_DB")
    cubemx_db = cubemx_db_list[0] if cubemx_db_list else Path("~/STM32CubeMX/db").expanduser()
    cmsis_device_dirs = _env_paths("POPCORN_CMSIS_DEVICE_DIRS")
    return SearchPaths(svd_dirs=svd_dirs, cubemx_db=cubemx_db, cmsis_device_dirs=cmsis_device_dirs)


def match_refname(part: str, refnames: Sequence[str], *, file: str) -> str | None:
    """The one of @p refnames that @p part names: exact, then @p part read as a CPN matched
    against every RefName's 'x' wildcard positions, each 'x' matching any one character of
    @p part. None when none matches, DescriptionError listing every candidate when several do."""
    if part in refnames:
        return part
    candidates = [
        ref
        for ref in refnames
        if len(ref) == len(part) and all(r == "x" or r == p for r, p in zip(ref, part, strict=True))
    ]
    if len(candidates) > 1:
        raise DescriptionError(
            file, 0, part, f"exactly one match expected, found {', '.join(candidates)}"
        )
    return candidates[0] if candidates else None


def resolve_part(part: str, db_dir: Path) -> str:
    """db RefName for @p part, by match_refname over the db's part files, DescriptionError with
    difflib close matches when none match, listing every candidate when several do."""
    mcu_dir = db_dir / "mcu"
    refnames = sorted(p.stem for p in mcu_dir.glob("*.xml"))
    found = match_refname(part, refnames, file=str(db_dir))
    if found is not None:
        return found
    close = difflib.get_close_matches(part, refnames, n=5)
    if close:
        raise DescriptionError(
            str(db_dir), 0, part, f"a known part, close matches: {', '.join(close)}"
        )
    raise DescriptionError(str(db_dir), 0, part, "a known part")


def _wildcard_prefix_matches(pattern: str, text: str) -> bool:
    if len(text) < len(pattern):
        return False
    return all(p == "x" or p == text[i] for i, p in enumerate(pattern))


def resolve_svd_stem(
    refname: str, svd_dirs: Sequence[Path], *, core: str | None, file: str
) -> tuple[str, Path]:
    stem_dirs: dict[str, Path] = {}
    for svd_dir in svd_dirs:
        for svd_file in sorted(svd_dir.glob("*.svd")):
            stem_dirs.setdefault(svd_file.stem, svd_dir)
    groups: dict[str, list[str]] = {}
    for stem in stem_dirs:
        base = _CORE_SUFFIX_RE.sub("", stem)
        groups.setdefault(base, []).append(stem)
    matches = sorted(
        (base for base in groups if _wildcard_prefix_matches(base, refname)),
        key=len,
        reverse=True,
    )
    if not matches:
        raise DescriptionError(file, 0, refname, "an SVD file stem matching this part")
    variants = sorted(groups[matches[0]])
    if len(variants) == 1:
        stem = variants[0]
        return stem, stem_dirs[stem]
    if core is not None:
        for stem in variants:
            if core in stem:
                return stem, stem_dirs[stem]
        raise DescriptionError(file, 0, refname, f"a core matching one of {', '.join(variants)}")
    raise DescriptionError(file, 0, refname, f"core=, candidates {', '.join(variants)}")


def _patch_files() -> list[Path]:
    if not _PATCHES_DIR.is_dir():
        return []
    return sorted(_PATCHES_DIR.glob("*.toml"))


def _svd_exception(refname: str, *, search: SearchPaths) -> tuple[str, Path] | None:
    for loaded in load_patches(_patch_files(), part=refname, svd_name=""):
        if loaded.patch.svd is not None:
            name = loaded.patch.svd.name
            for svd_dir in search.svd_dirs:
                if (svd_dir / f"{name}.svd").is_file():
                    return name, svd_dir
            return name, search.svd_dirs[0] if search.svd_dirs else Path()
    return None


def _cmsis_header_path(refname: str, search: SearchPaths) -> Path | None:
    stem = f"{refname[:9].lower()}xx.h"
    if search.cmsis_device_dirs is not None:
        for device_dir in search.cmsis_device_dirs:
            candidate = device_dir / stem
            if candidate.is_file():
                return candidate
        return None
    if len(refname) < 7:
        return None
    family = refname[5:7]
    repo = Path("~/STM32Cube/Repository").expanduser()
    for fw_dir in sorted(repo.glob(f"STM32Cube_FW_{family}_V*"), key=_version_key, reverse=True):
        candidate = (
            fw_dir / "Drivers" / "CMSIS" / "Device" / "ST" / f"STM32{family}xx" / "Include" / stem
        )
        if candidate.is_file():
            return candidate
    return None


def _require(path: Path, file: str, key: str, expected: str) -> None:
    if absent(path):
        raise MissingSource(file, key, expected, path=path)
    if not path.is_file():
        raise DescriptionError(file, 0, key, expected)


def _svd_dirs(svd_dirs: Sequence[Path]) -> list[Path]:
    present = [svd_dir for svd_dir in svd_dirs if not absent(svd_dir)]
    if present:
        return present
    if svd_dirs:
        missing = svd_dirs[0]
        source = "POPCORN_STM32_SVD_DIRS or the search paths"
    else:
        st_root = Path("~/st").expanduser()
        missing = st_root if absent(st_root) else st_root / "stm32cubeclt_*"
        source = f"an ST install under {st_root} or POPCORN_STM32_SVD_DIRS"
    raise MissingSource(
        "<search>",
        "svd_dirs",
        f"at least one existing SVD directory to search, from {source}",
        path=missing,
    )


def _svd_path(
    refname: str, *, core: str | None, svd: Path | None, search: SearchPaths, file: str
) -> Path:
    if svd is not None:
        return svd
    exception = _svd_exception(refname, search=search)
    if exception is not None:
        name, svd_dir = exception
        svd_path = svd_dir / f"{name}.svd"
        _require(svd_path, str(svd_dir), refname, f"an SVD file at {svd_path}")
        return svd_path
    stem, svd_dir = resolve_svd_stem(refname, _svd_dirs(search.svd_dirs), core=core, file=file)
    return svd_dir / f"{stem}.svd"


def find_sources(
    part: str,
    *,
    core: str | None = None,
    svd: Path | None = None,
    cubemx_db: Path | None = None,
    cmsis_header: Path | None = None,
    search: SearchPaths | None = None,
) -> Sources:
    """Sources of @p part: db files by exact db lookup, SVD by family-prefix matching across
    @p search's directories (or the spec 04 defaults, when @p search is None), CMSIS header
    optionally, every patch file in the package whose [match] selects this part and the resolved
    SVD file name, and the GPIO modes file of the part's GPIO IP version when it exists, None
    otherwise. MissingSource, a MissingInput and a DescriptionError, naming the path when a
    required source does not exist: no CubeMX db, no existing SVD directory, an absent db, rzone
    or SVD file. DescriptionError for an unknown part in an existing db, or a source that exists
    but is not a file."""
    search = search or default_search_paths()
    db_dir = cubemx_db or search.cubemx_db
    if absent(db_dir / "mcu"):
        raise MissingSource(
            str(db_dir),
            "cubemx_db",
            f"a CubeMX db with an mcu directory at {db_dir}, set POPCORN_CUBEMX_DB",
            path=db_dir,
        )
    refname = resolve_part(part, db_dir)

    mcu_db = db_dir / "mcu" / f"{refname}.xml"
    _require(mcu_db, str(db_dir), refname, f"a db file at {mcu_db}")
    db_part = read_part(mcu_db.read_bytes(), file=str(mcu_db))

    rzone = db_dir / "mcu" / "memory" / rzone_file_name(db_part)
    _require(rzone, str(db_dir), refname, f"an rzone file at {rzone}")
    modes_name = gpio_modes_file_name(db_part)
    gpio_modes = db_dir / "mcu" / "IP" / modes_name if modes_name is not None else None
    if gpio_modes is not None and not gpio_modes.is_file():
        gpio_modes = None

    svd_path = _svd_path(refname, core=core, svd=svd, search=search, file=str(db_dir))
    cmsis_header_path = (
        cmsis_header if cmsis_header is not None else _cmsis_header_path(refname, search)
    )

    patches = load_patches(_patch_files(), part=refname, svd_name=svd_path.name)

    return Sources(
        part=refname,
        svd=svd_path,
        mcu_db=mcu_db,
        rzone=rzone,
        cmsis_header=cmsis_header_path,
        patches=[loaded.path for loaded in patches],
        gpio_modes=gpio_modes,
    )


def pin_data_sources(
    part: str,
    *,
    pin_data: Path,
    core: str | None = None,
    svd: Path | None = None,
    cmsis_header: Path | None = None,
    search: SearchPaths | None = None,
) -> Sources:
    """Sources of @p part for a shipped description: the part and GPIO modes files of a
    STM32_open_pin_data checkout at @p pin_data, laid out as a CubeMX db's `mcu/` and `mcu/IP/`,
    the packaged rzone facts file in place of the rzone, and the SVD, CMSIS header and patches
    as find_sources finds them. Reads no file of the CubeMX db. MissingSource naming the path
    when the checkout, the part or modes file, or the SVD does not exist. DescriptionError for
    an unknown part, a part without a GPIO IP version, or no packaged facts file for its rzone."""
    search = search or default_search_paths()
    if absent(pin_data / "mcu"):
        raise MissingSource(
            str(pin_data),
            "pin_data",
            f"a STM32_open_pin_data checkout with an mcu directory at {pin_data}",
            path=pin_data,
        )
    refname = resolve_part(part, pin_data)
    mcu_db = pin_data / "mcu" / f"{refname}.xml"
    _require(mcu_db, str(pin_data), refname, f"a part file at {mcu_db}")
    db_part = read_part(mcu_db.read_bytes(), file=str(mcu_db))
    modes_name = gpio_modes_file_name(db_part)
    if modes_name is None:
        raise DescriptionError(str(mcu_db), 0, "GPIO", "a GPIO IP with a version")
    gpio_modes = pin_data / "mcu" / "IP" / modes_name
    _require(gpio_modes, str(pin_data), refname, f"a GPIO modes file at {gpio_modes}")
    facts = facts_path(db_part)
    if not facts.is_file():
        raise DescriptionError(str(mcu_db), 0, refname, f"a packaged rzone facts file at {facts}")
    svd_path = _svd_path(refname, core=core, svd=svd, search=search, file=str(pin_data))
    cmsis_header_path = (
        cmsis_header if cmsis_header is not None else _cmsis_header_path(refname, search)
    )
    patches = load_patches(_patch_files(), part=refname, svd_name=svd_path.name)
    return Sources(
        part=refname,
        svd=svd_path,
        mcu_db=mcu_db,
        rzone=None,
        cmsis_header=cmsis_header_path,
        patches=[loaded.path for loaded in patches],
        gpio_modes=gpio_modes,
        rzone_facts=facts,
    )


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def compute_source_digests(sources: Sources) -> dict[str, str]:
    """SHA-256 hex digest of each of @p sources's files, keyed svd and mcu_db, rzone when set
    or else rzone_facts when set, cmsis_header and gpio_modes when present, and patch:<file
    name> per patch file."""
    digests = {
        "svd": _sha256_bytes(sources.svd.read_bytes()),
        "mcu_db": _sha256_bytes(sources.mcu_db.read_bytes()),
    }
    if sources.rzone is not None:
        digests["rzone"] = _sha256_bytes(sources.rzone.read_bytes())
    elif sources.rzone_facts is not None:
        digests["rzone_facts"] = _sha256_bytes(sources.rzone_facts.read_bytes())
    if sources.cmsis_header is not None:
        digests["cmsis_header"] = _sha256_bytes(sources.cmsis_header.read_bytes())
    if sources.gpio_modes is not None:
        digests["gpio_modes"] = _sha256_bytes(sources.gpio_modes.read_bytes())
    for patch_path in sources.patches:
        digests[f"patch:{patch_path.name}"] = _sha256_bytes(patch_path.read_bytes())
    return digests


def facts_path(part: DbPart) -> Path:
    """Path of the packaged rzone facts file standing for the CubeMX rzone of @p part,
    `rzone/<rzone file stem>.toml`, whether or not it exists."""
    return _RZONE_FACTS_DIR / f"{Path(rzone_file_name(part)).stem}.toml"


def builder_digest() -> str:
    """SHA-256 over this package's own .py files, patches/*.toml and rzone/*.toml bytes, sorted
    path order, so a builder edit invalidates the cache without a version bump."""
    package_dir = Path(__file__).parent
    paths = sorted(
        {*package_dir.rglob("*.py"), *_PATCHES_DIR.glob("*.toml"), *_RZONE_FACTS_DIR.glob("*.toml")}
    )
    hasher = hashlib.sha256()
    for path in paths:
        hasher.update(str(path.relative_to(package_dir)).encode())
        hasher.update(path.read_bytes())
    return hasher.hexdigest()


def generator_string() -> str:
    """'popcorn.stm32.description <popcorn version> <builder digest[:12]>'."""
    version = importlib.metadata.version("popcorn")
    return f"popcorn.stm32.description {version} {builder_digest()[:12]}"
