from pathlib import Path

import pytest
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._sources import (
    SearchPaths,
    default_svd_dirs,
    find_sources,
    resolve_part,
    resolve_svd_stem,
)

DATA_DIR = Path(__file__).parents[3] / "tests" / "data" / "stm32"
SVD_NAMES = (DATA_DIR / "svd_names.txt").read_text(encoding="utf-8").splitlines()


def _synthetic_svd_dir(tmp_path: Path) -> Path:
    svd_dir = tmp_path / "svd"
    svd_dir.mkdir()
    for name in SVD_NAMES:
        (svd_dir / name).write_text("<device/>", encoding="utf-8")
    return svd_dir


def test_resolve_part_exact_and_cpn(st_cubemx_db: Path) -> None:
    assert resolve_part("STM32U575QGIx", st_cubemx_db) == "STM32U575QGIx"
    assert resolve_part("STM32U575QGI6", st_cubemx_db) == "STM32U575QGIx"


def test_resolve_part_unknown_lists_close_matches(st_cubemx_db: Path) -> None:
    with pytest.raises(DescriptionError) as excinfo:
        resolve_part("STM32U575QGIZZ", st_cubemx_db)
    assert "STM32U575QGIx" in excinfo.value.expected


def test_resolve_part_two_matches_is_an_error(tmp_path: Path) -> None:
    db_dir = tmp_path / "db"
    mcu_dir = db_dir / "mcu"
    mcu_dir.mkdir(parents=True)
    (mcu_dir / "AAxA.xml").write_text("<Mcu/>", encoding="utf-8")
    (mcu_dir / "AxAA.xml").write_text("<Mcu/>", encoding="utf-8")

    with pytest.raises(DescriptionError) as excinfo:
        resolve_part("AAAA", db_dir)
    assert "AAxA" in excinfo.value.expected
    assert "AxAA" in excinfo.value.expected


def test_svd_stem_u575(tmp_path: Path) -> None:
    svd_dir = _synthetic_svd_dir(tmp_path)
    stem, found_dir = resolve_svd_stem("STM32U575QGIx", [svd_dir], core=None, file="test")
    assert stem == "STM32U575"
    assert found_dir == svd_dir


def test_svd_stem_wildcard_family(tmp_path: Path) -> None:
    svd_dir = _synthetic_svd_dir(tmp_path)
    stem, _ = resolve_svd_stem("STM32F030F4Px", [svd_dir], core=None, file="test")
    assert stem == "STM32F0x0"


def test_svd_stem_dual_core_needs_core(tmp_path: Path) -> None:
    svd_dir = _synthetic_svd_dir(tmp_path)
    with pytest.raises(DescriptionError) as excinfo:
        resolve_svd_stem("STM32H745ZITx", [svd_dir], core=None, file="test")
    assert "STM32H745_CM4" in excinfo.value.expected
    assert "STM32H745_CM7" in excinfo.value.expected

    stem, _ = resolve_svd_stem("STM32H745ZITx", [svd_dir], core="CM7", file="test")
    assert stem == "STM32H745_CM7"


def test_svd_stem_wba52(tmp_path: Path) -> None:
    svd_dir = _synthetic_svd_dir(tmp_path)
    stem, _ = resolve_svd_stem("STM32WBA52CGUx", [svd_dir], core=None, file="test")
    assert stem == "STM32WBA52"


def test_newest_clt_picked_by_version_not_lexical_order(tmp_path: Path) -> None:
    st_root = tmp_path / "st"
    for version in ("1.9.0", "1.19.0", "1.20.0"):
        svd_dir = st_root / f"stm32cubeclt_{version}" / "STMicroelectronics_CMSIS_SVD"
        svd_dir.mkdir(parents=True)
        (svd_dir / "MARKER.svd").write_text(version, encoding="utf-8")

    dirs = default_svd_dirs(st_root)
    assert len(dirs) == 3
    assert (dirs[0] / "MARKER.svd").read_text(encoding="utf-8") == "1.20.0"
    assert (dirs[1] / "MARKER.svd").read_text(encoding="utf-8") == "1.19.0"
    assert (dirs[2] / "MARKER.svd").read_text(encoding="utf-8") == "1.9.0"


def test_find_sources_missing_everything_lists_every_searched_path(tmp_path: Path) -> None:
    empty_db = tmp_path / "db"
    (empty_db / "mcu").mkdir(parents=True)
    search = SearchPaths(svd_dirs=[], cubemx_db=empty_db, cmsis_device_dirs=None)
    with pytest.raises(DescriptionError):
        find_sources("STM32U575QGIx", search=search)


def test_find_sources_real_u575(st_svd_u575: Path, st_cubemx_db: Path) -> None:
    sources = find_sources("STM32U575QGIx")
    assert sources.part == "STM32U575QGIx"
    assert sources.svd == st_svd_u575
    assert sources.mcu_db == st_cubemx_db / "mcu" / "STM32U575QGIx.xml"
    assert sources.rzone.name == "STM32_DIE482_786_1024.xml"
    assert sources.cmsis_header is not None
    assert sources.cmsis_header.name == "stm32u575xx.h"
    assert len(sources.patches) == 2
