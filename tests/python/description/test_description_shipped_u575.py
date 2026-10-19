"""Shipped descriptions against a fresh ship build, when STM32_open_pin_data and ST's files are
installed. A builder, patch or facts change that changes a shipped part's content fails here
until `python -m popcorn.stm32.description ship` reruns."""

import json
import os
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn.stm32.description.__main__ import main
from popcorn.stm32.description._errors import MissingSource
from popcorn.stm32.description._shipped import (
    SHIPPED_DIR,
    build_shipped,
    shipped_names,
    shipped_text,
)
from popcorn.stm32.description._sources import compute_source_digests, pin_data_sources

PIN_DATA_VARIABLE = "POPCORN_STM32_PIN_DATA"
GOLDEN_DIR = Path(__file__).parents[3] / "tests" / "data" / "stm32" / "golden"
EXTERNAL_SOURCES = ("svd", "mcu_db", "cmsis_header", "gpio_modes")
"""Source digests of files popcorn does not own. A difference there means another ST install,
not a stale shipped file."""


def _pin_data() -> Path:
    value = os.environ.get(PIN_DATA_VARIABLE)
    if not value:
        pytest.skip(f"set {PIN_DATA_VARIABLE} to a STM32_open_pin_data checkout")
    return Path(value)


def _sources_of(path: Path) -> dict[str, str]:
    data = cast(dict[str, object], json.loads(shipped_text(path.read_bytes(), file=str(path))))
    return cast(dict[str, str], data["sources"])


def test_a_part_ships_exactly_when_it_has_a_golden() -> None:
    assert shipped_names() == sorted(path.stem for path in GOLDEN_DIR.glob("*.json"))


@pytest.mark.parametrize("name", shipped_names())
def test_the_shipped_description_matches_a_fresh_ship_build(name: str) -> None:
    pin_data = _pin_data()
    path = SHIPPED_DIR / f"{name}.json.xz"
    part, _, core = name.partition("-")
    try:
        sources = pin_data_sources(part, pin_data=pin_data, core=core or None)
    except MissingSource as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    local = compute_source_digests(sources)
    shipped_sources = _sources_of(path)
    differing = [key for key in EXTERNAL_SOURCES if local.get(key) != shipped_sources.get(key)]
    if differing:
        pytest.skip(f"{name}: local {', '.join(differing)} differ from the shipped file's")
    _, packed = build_shipped(part, pin_data=pin_data, core=core or None)
    fresh = popcorn.parse_description(shipped_text(packed, file="fresh"), "fresh")
    shipped = popcorn.parse_description(shipped_text(path.read_bytes(), file=str(path)), str(path))
    assert popcorn.description_digest(fresh) == popcorn.description_digest(shipped)


def test_ship_refuses_a_part_without_a_cmsis_header(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    pin_data = _pin_data()
    try:
        svd = pin_data_sources("STM32U575QGIx", pin_data=pin_data).svd
    except MissingSource as exc:
        pytest.skip(f"STM32U575QGIx sources not found: {exc}")
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.setenv("POPCORN_STM32_SVD_DIRS", str(svd.parent))
    empty = tmp_path / "cmsis"
    empty.mkdir()
    monkeypatch.setenv("POPCORN_CMSIS_DEVICE_DIRS", str(empty))
    out = tmp_path / "out"
    status = main(["ship", "STM32U575QGIx", "--pin-data", str(pin_data), "--out", str(out)])
    assert status == 1
    assert "cmsis_header" in capsys.readouterr().err
    assert not out.exists()


def test_ship_refuses_to_write_without_out(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    status = main(["ship", "STM32U575QGIx", "--pin-data", str(tmp_path / "absent")])
    assert status == 1
    assert "--out is required" in capsys.readouterr().err
