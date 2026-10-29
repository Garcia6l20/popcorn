"""Shipped descriptions against a fresh ship build, when STM32_open_pin_data and ST's files are
installed. A builder, patch or facts change that changes a shipped part's content fails here
until `python -m popcorn.stm32.description ship` reruns."""

import json
import lzma
import os
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn.stm32.description import __main__ as cli
from popcorn.stm32.description.__main__ import main
from popcorn.stm32.description._cubemx import bonded_pins, read_part
from popcorn.stm32.description._errors import DescriptionError, MissingSource
from popcorn.stm32.description._layers import choose_reference, factor
from popcorn.stm32.description._shipped import (
    SHIPPED_DIR,
    build_layers,
    build_text,
    compose_shipped,
    shipped_names,
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


def _sources_of(name: str) -> dict[str, str]:
    data = cast(dict[str, object], json.loads(compose_shipped(name)))
    return cast(dict[str, str], data["sources"])


def test_a_part_ships_exactly_when_it_has_a_golden() -> None:
    assert shipped_names() == sorted(path.stem for path in GOLDEN_DIR.glob("*.json"))


@pytest.mark.parametrize("name", shipped_names())
def test_the_shipped_description_matches_a_fresh_ship_build(name: str) -> None:
    pin_data = _pin_data()
    part, _, core = name.partition("-")
    try:
        sources = pin_data_sources(part, pin_data=pin_data, core=core or None)
    except MissingSource as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    local = compute_source_digests(sources)
    shipped_sources = _sources_of(name)
    stale = [
        key for key in local if key.startswith("patch:") and local[key] != shipped_sources.get(key)
    ]
    assert not stale, f"{name}: shipped patch digests differ, rerun ship: {stale}"
    assert {k for k in shipped_sources if k.startswith("patch:")} == {
        k for k in local if k.startswith("patch:")
    }
    differing = [key for key in EXTERNAL_SOURCES if local.get(key) != shipped_sources.get(key)]
    if differing:
        pytest.skip(f"{name}: local {', '.join(differing)} differ from the shipped file's")
    fresh = popcorn.parse_description(build_text(name, pin_data=pin_data), "fresh")
    shipped = popcorn.parse_description(compose_shipped(name), name)
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
    status = main(["ship", "STM32U575QGIx", "--pin-data", str(pin_data), "--check"])
    assert status == 1
    assert "cmsis_header" in capsys.readouterr().err


TWO_PARTS = {"STM32U575xx": "STM32U575QGIx", "STM32H573xx": "STM32H573AIIx"}
REFERENCES = TWO_PARTS
"""Reference of each shipped line, the choice of `_layers.choose_reference` over its shipped
parts."""


def test_each_shipped_line_has_the_reference_the_rule_picks_from_the_db() -> None:
    pin_data = _pin_data()
    ranks: dict[str, tuple[int, int]] = {}
    for name in shipped_names():
        part, _, core = name.partition("-")
        try:
            sources = pin_data_sources(part, pin_data=pin_data, core=core or None)
        except MissingSource as exc:
            pytest.skip(f"{name} sources not found: {exc}")
        db_part = read_part(sources.mcu_db.read_bytes(), file=str(sources.mcu_db))
        bonded = sum(mask.bit_count() for mask in bonded_pins(db_part).values())
        ranks[name] = (bonded, db_part.flash_kib[0])
    assert choose_reference(ranks) == REFERENCES
    for path in SHIPPED_DIR.glob("*.line.json.xz"):
        line = cast(dict[str, object], json.loads(lzma.decompress(path.read_bytes())))
        assert REFERENCES[cast(str, line["line"])] == line["reference"]


@pytest.mark.timeout(60)
def test_a_ship_build_in_a_process_pool_writes_the_shipped_blocks_and_lines() -> None:
    pin_data = _pin_data()
    for part in TWO_PARTS.values():
        try:
            pin_data_sources(part, pin_data=pin_data)
        except MissingSource as exc:
            pytest.skip(f"{part} sources not found: {exc}")
    names = sorted(TWO_PARTS.values())
    files = build_layers(names, pin_data=pin_data, jobs=2)
    expected = factor({name: json.loads(compose_shipped(name)) for name in names}, TWO_PARTS)
    assert sorted(files) == sorted(expected)
    for file in files:
        if not file.endswith(".parts.json.xz"):
            assert lzma.decompress(files[file]) == lzma.decompress(expected[file]), file


def test_failed_builds_in_a_process_pool_raise_the_first_failure_in_name_order() -> None:
    pin_data = _pin_data()
    with pytest.raises(DescriptionError) as excinfo:
        build_layers(["STM32U575QQQx", "STM32U575QZZx"], pin_data=pin_data, jobs=2)
    assert excinfo.value.key == "STM32U575QQQx"


@pytest.mark.timeout(60)
def test_golden_without_a_part_checks_every_golden(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    u575_sources: object,
    h573_sources: object,
) -> None:
    goldens = tmp_path / "golden"
    goldens.mkdir()
    for name in TWO_PARTS.values():
        (goldens / f"{name}.json").write_bytes((GOLDEN_DIR / f"{name}.json").read_bytes())
    tampered = goldens / "STM32H573AIIx.json"
    data = json.loads(tampered.read_text())
    data["counts"]["aliases"] += 1
    tampered.write_text(json.dumps(data))
    monkeypatch.setattr(cli, "GOLDEN_DIR", goldens)
    assert main(["golden"]) == 1
    out, err = capsys.readouterr()
    assert out.split() == [str(goldens / "STM32U575QGIx.json")]
    assert f"{tampered}: does not match" in err


@pytest.mark.parametrize("name", shipped_names())
def test_ship_check_passes_on_the_shipped_layers(
    name: str, capsys: pytest.CaptureFixture[str]
) -> None:
    pin_data = _pin_data()
    try:
        pin_data_sources(name, pin_data=pin_data)
    except MissingSource as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    assert main(["ship", name, "--pin-data", str(pin_data), "--check"]) == 0, capsys.readouterr()


def test_the_shipped_h573_sec_usb_keeps_the_patched_usb_field_kinds() -> None:
    data = cast(dict[str, object], json.loads(compose_shipped("STM32H573AIIx")))
    peripherals = cast(list[dict[str, object]], data["peripherals"])
    by_name = {cast(str, p["name"]): p for p in peripherals}
    secure = by_name["SEC_USB"]
    assert secure["alias_of"] == "USB"
    kinds: dict[str, dict[str, tuple[object, object]]] = {}
    for unit in ("USB", "SEC_USB"):
        registers = cast(list[dict[str, object]], by_name[unit]["registers"])
        kinds[unit] = {
            f"{r['name']}.{f['name']}": (f.get("on_write"), f.get("acc"))
            for r in registers
            if cast(str, r["name"]) in {f"CHEP{n}R" for n in range(8)} | {"ISTR"}
            for f in cast(list[dict[str, object]], r["fields"])
        }
    assert kinds["SEC_USB"] == kinds["USB"]
    assert kinds["SEC_USB"]["CHEP3R.DTOGRX"] == ("one_to_toggle", "read_write")
    assert kinds["SEC_USB"]["CHEP3R.VTRX"][0] == "zero_to_clear"
    assert kinds["SEC_USB"]["ISTR.RST_DCON"][0] == "zero_to_clear"
