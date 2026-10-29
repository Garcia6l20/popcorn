"""`golden` without a part list checks every golden file."""

import json
from pathlib import Path

import pytest
from popcorn.stm32.description import __main__ as cli

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
