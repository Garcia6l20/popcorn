import json
from pathlib import Path

import pytest
from popcorn.stm32.description._shipped import compose_shipped

from popcorn_build.python import package_files
from popcorn_build.st_description import cached_st_description

M33_JSON = Path(__file__).parents[3] / "tests" / "data" / "core" / "m33.json"
NAME = "STM32T001ABCx"


def test_the_build_probe_takes_the_lookup_of_mcu(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    directory = tmp_path / "descriptions"
    directory.mkdir()
    raw = json.loads(M33_JSON.read_text())
    (directory / f"{NAME}.json").write_text(json.dumps(raw | {"part": NAME}))
    absent = tmp_path / "absent"
    monkeypatch.setenv("HOME", str(absent))
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(absent / "db"))
    monkeypatch.setenv("POPCORN_STM32_SVD_DIRS", str(absent / "svd"))
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    monkeypatch.setenv("POPCORN_STM32_DESCRIPTION_DIRS", str(directory))
    monkeypatch.delenv("POPCORN_STM32_BUILD", raising=False)
    monkeypatch.delenv("POPCORN_PROBE_UNSET", raising=False)
    assert (
        cached_st_description("STM32T001ABC6", "POPCORN_PROBE_UNSET") == directory / f"{NAME}.json"
    )


def test_the_build_probe_composes_a_shipped_part_without_the_extension(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    absent = tmp_path / "absent"
    monkeypatch.setenv("HOME", str(absent))
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(absent / "db"))
    monkeypatch.setenv("POPCORN_STM32_SVD_DIRS", str(absent / "svd"))
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    monkeypatch.delenv("POPCORN_STM32_DESCRIPTION_DIRS", raising=False)
    monkeypatch.delenv("POPCORN_STM32_BUILD", raising=False)
    monkeypatch.delenv("POPCORN_PROBE_UNSET", raising=False)
    path = cached_st_description("STM32U575QGIx", "POPCORN_PROBE_UNSET")
    assert path is not None
    assert path.parent == tmp_path / "cache" / "popcorn" / "mcu" / "shipped"
    assert path.read_text() == compose_shipped("STM32U575QGIx")


def test_package_data_includes_shipped_layers(tmp_path: Path) -> None:
    data = tmp_path / "stm32" / "data"
    data.mkdir(parents=True)
    layers = [
        data / name
        for name in (
            "STM32T001xx.line.json.xz",
            "STM32T001xx.parts.json.xz",
            "STM32T0xx.blocks.json.xz",
        )
    ]
    for layer in layers:
        layer.write_bytes(b"")
    (data / "notes.json").write_text("{}")
    assert package_files(tmp_path) == layers
