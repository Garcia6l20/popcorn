import json
from pathlib import Path

import pytest

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


def test_package_data_includes_shipped_descriptions(tmp_path: Path) -> None:
    shipped = tmp_path / "stm32" / "data" / f"{NAME}.json.xz"
    shipped.parent.mkdir(parents=True)
    shipped.write_bytes(b"")
    (tmp_path / "stm32" / "data" / "notes.json").write_text("{}")
    assert package_files(tmp_path) == [shipped]
