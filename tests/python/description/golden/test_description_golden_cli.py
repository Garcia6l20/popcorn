"""`golden` without a part list checks every golden file."""

import json
from pathlib import Path

import pytest
from popcorn.stm32.description import __main__ as cli
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._sources import find_sources

GOLDEN_DIR = Path(__file__).parents[4] / "tests" / "data" / "stm32" / "golden"
PARTS = ("STM32U575QGIx", "STM32H573AIIx")


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
    for name in PARTS:
        (goldens / f"{name}.json").write_bytes((GOLDEN_DIR / f"{name}.json").read_bytes())
    tampered = goldens / "STM32H573AIIx.json"
    data = json.loads(tampered.read_text())
    data["counts"]["aliases"] += 1
    tampered.write_text(json.dumps(data))
    monkeypatch.setattr(cli, "GOLDEN_DIR", goldens)
    assert cli.main(["golden"]) == 1
    out, err = capsys.readouterr()
    assert out.split() == [str(goldens / "STM32U575QGIx.json")]
    assert f"{tampered}: does not match" in err


@pytest.mark.timeout(60)
def test_golden_update_refuses_a_part_without_a_cmsis_header_and_keeps_its_golden(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    name = "STM32L152RETx"
    try:
        find_sources(name)
    except DescriptionError as exc:
        pytest.skip(f"{name} sources not found: {exc}")
    goldens = tmp_path / "golden"
    goldens.mkdir()
    golden = goldens / f"{name}.json"
    original = (GOLDEN_DIR / f"{name}.json").read_bytes()
    golden.write_bytes(original)
    empty = tmp_path / "cmsis"
    empty.mkdir()
    monkeypatch.setenv("POPCORN_CMSIS_DEVICE_DIRS", str(empty))
    monkeypatch.setattr(cli, "GOLDEN_DIR", goldens)
    assert cli.main(["golden", name, "--update"]) == 1
    assert "cmsis_header" in capsys.readouterr().err
    assert golden.read_bytes() == original
