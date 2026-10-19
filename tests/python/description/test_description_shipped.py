"""Shipped descriptions and the lookup order of description_path, with no ST file reachable."""

import json
import lzma
import time
from collections.abc import Callable
from pathlib import Path

import popcorn
import pytest
from popcorn.stm32.description import _shipped
from popcorn.stm32.description._errors import DescriptionError, MissingSource
from popcorn.stm32.description._shipped import description_path, shipped_description

M33_JSON = Path(__file__).parents[3] / "tests" / "data" / "core" / "m33.json"
NAME = "STM32T001ABCx"


def _description_bytes(part: str = NAME, generator: str = "hand-written") -> bytes:
    raw = json.loads(M33_JSON.read_text())
    return json.dumps(raw | {"part": part, "generator": generator}).encode()


@pytest.fixture
def shipped_dir(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    directory = tmp_path / "data"
    directory.mkdir()
    (directory / f"{NAME}.json.xz").write_bytes(lzma.compress(_description_bytes()))
    monkeypatch.setattr(_shipped, "SHIPPED_DIR", directory)
    absent = tmp_path / "absent"
    monkeypatch.setenv("HOME", str(absent))
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(absent / "db"))
    monkeypatch.setenv("POPCORN_STM32_SVD_DIRS", str(absent / "svd"))
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    monkeypatch.delenv("POPCORN_STM32_DESCRIPTION_DIRS", raising=False)
    monkeypatch.delenv("POPCORN_STM32_BUILD", raising=False)
    return directory


def _cached(tmp_path: Path) -> list[Path]:
    return sorted((tmp_path / "cache" / "popcorn" / "mcu" / "shipped").glob("*.json"))


def test_a_shipped_part_resolves_with_no_st_files(shipped_dir: Path, tmp_path: Path) -> None:
    exact = description_path(NAME)
    assert exact.parent == tmp_path / "cache" / "popcorn" / "mcu" / "shipped"
    assert exact.read_bytes() == _description_bytes()
    assert popcorn.load_description(exact).part == NAME
    assert description_path("STM32T001ABC6") == exact


def test_a_second_lookup_reuses_the_decompressed_file(
    shipped_dir: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    calls: list[int] = []
    real: Callable[..., bytes] = _shipped.bounded_xz

    def counting(data: bytes, limit: int, *, file: str) -> bytes:
        calls.append(len(data))
        return real(data, limit, file=file)

    monkeypatch.setattr(_shipped, "bounded_xz", counting)
    first = description_path(NAME)
    assert description_path(NAME) == first
    assert len(calls) == 1


def test_a_changed_shipped_file_replaces_the_decompressed_one(
    shipped_dir: Path, tmp_path: Path
) -> None:
    packed = [lzma.compress(_description_bytes(generator=g)) for g in ("a", "b")]
    paths: set[Path] = set()
    for index in range(1000):
        (shipped_dir / f"{NAME}.json.xz").write_bytes(packed[index % 2])
        path = description_path(NAME)
        paths.add(path)
        assert _cached(tmp_path) == [path]
    assert len(paths) == 2


def test_a_core_suffixed_shipped_file_answers_only_for_its_core(shipped_dir: Path) -> None:
    (shipped_dir / f"{NAME}.json.xz").rename(shipped_dir / f"{NAME}-CM4.json.xz")
    assert shipped_description(NAME) is None
    assert shipped_description(NAME, core="CM7") is None
    found = shipped_description(NAME, core="CM4")
    assert found is not None
    assert found.name.startswith(f"{NAME}-CM4-")


def test_a_description_directory_wins_over_the_shipped_file(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    first = tmp_path / "first"
    second = tmp_path / "second"
    for directory in (first, second):
        directory.mkdir()
        (directory / f"{NAME}.json").write_bytes(_description_bytes(generator=directory.name))
    monkeypatch.setenv(
        "POPCORN_STM32_DESCRIPTION_DIRS", f"{tmp_path / 'absent-dir'}:{first}:{second}"
    )
    assert description_path("STM32T001ABC6") == first / f"{NAME}.json"
    assert _cached(tmp_path) == []


def test_svd_skips_the_shipped_file(shipped_dir: Path, tmp_path: Path) -> None:
    with pytest.raises(MissingSource):
        description_path(NAME, svd=tmp_path / "absent.svd")
    assert _cached(tmp_path) == []


def test_a_local_build_request_skips_the_shipped_file(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("POPCORN_STM32_BUILD", "local")
    with pytest.raises(MissingSource):
        description_path(NAME)
    assert _cached(tmp_path) == []


def test_an_unknown_build_request_is_refused_before_any_lookup(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    directory = tmp_path / "dir"
    directory.mkdir()
    (directory / f"{NAME}.json").write_bytes(_description_bytes())
    monkeypatch.setenv("POPCORN_STM32_DESCRIPTION_DIRS", str(directory))
    monkeypatch.setenv("POPCORN_STM32_BUILD", "shipped")
    with pytest.raises(DescriptionError) as excinfo:
        description_path(NAME)
    assert excinfo.value.key == "POPCORN_STM32_BUILD"


def test_an_unshipped_part_without_st_files_names_the_shipped_parts(shipped_dir: Path) -> None:
    with pytest.raises(MissingSource) as excinfo:
        description_path("STM32T002ABCx")
    assert f"a shipped description, shipped: {NAME}, or ST's files" in excinfo.value.expected


def test_a_shipped_file_past_the_cap_is_refused_quickly(shipped_dir: Path, tmp_path: Path) -> None:
    (shipped_dir / f"{NAME}.json.xz").write_bytes(
        lzma.compress(bytes(_shipped.MAX_SHIPPED_BYTES + (1 << 20)), preset=0)
    )
    started = time.perf_counter()
    with pytest.raises(DescriptionError) as excinfo:
        description_path(NAME)
    assert time.perf_counter() - started < 1.0
    assert excinfo.value.expected == f"at most {_shipped.MAX_SHIPPED_BYTES} bytes decompressed"
    assert _cached(tmp_path) == []


@pytest.mark.parametrize(
    ("data", "expected"),
    [
        (b"this is not an xz stream at all", "an xz stream"),
        (lzma.compress(b"{}")[:-8], "a complete xz stream"),
        (lzma.compress(b"{}") + b"tail", "one xz stream and nothing after it"),
    ],
)
def test_a_broken_shipped_file_is_refused(shipped_dir: Path, data: bytes, expected: str) -> None:
    (shipped_dir / f"{NAME}.json.xz").write_bytes(data)
    with pytest.raises(DescriptionError) as excinfo:
        description_path(NAME)
    assert excinfo.value.expected == expected
