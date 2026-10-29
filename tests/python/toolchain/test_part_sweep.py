import json
import sys
from pathlib import Path

import popcorn
import pytest
from popcorn.stm32 import mcu
from popcorn.stm32.description._sources import default_search_paths

from popcorn_build.part_sweep import LOCAL_SOURCES, db_parts, main, sweep, sweep_part

SHIPPED = ("STM32H573AIIx", "STM32U575QGIx")

FAKE_CHILD = """
import json, os, sys, time
part = sys.argv[1]
if part == "CRASH":
    sys.exit(3)
if part == "SLOW":
    time.sleep(30)
marks = os.environ.get("SWEEP_MARKS")
running = None
if marks:
    mark = os.path.join(marks, part)
    open(mark, "w").close()
    running = len(os.listdir(marks))
time.sleep(float(os.environ.get("SWEEP_DELAY_" + part, "0")))
if marks:
    running = max(running, len(os.listdir(marks)))
    os.remove(mark)
print("log line")
print("PART_SWEEP_ROW " + json.dumps({"part": "ignored", "error": None, "running": running}))
print("exit warning")
"""
FAKE = (sys.executable, "-c", FAKE_CHILD)


@pytest.fixture
def shipped_only(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    for name in LOCAL_SOURCES:
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(tmp_path / "absent"))


@pytest.mark.timeout(60)
@pytest.mark.usefixtures("shipped_only")
def test_shipped_parts_sweep_ok_with_their_shipped_digests() -> None:
    rows = sweep(list(SHIPPED), source="shipped", jobs=2)
    assert [row["part"] for row in rows] == list(SHIPPED)
    for row in rows:
        assert row["error"] is None
        assert row["description"] == {
            "ok": True,
            "digest": popcorn.description_digest(mcu(row["part"])),
            "error": None,
        }
        assert row["attach"] == {"tz0": "ok", "tz1": "ok"}
        assert row["bonded"] is None
        assert row["package"] is None
        assert row["db_error"] is None
        assert row["smps"] is False
        instances = row["instances"]
        assert 0 < instances["modelled_tz0"] <= instances["modelled_tz1"] <= instances["total"]


@pytest.mark.timeout(60)
@pytest.mark.usefixtures("shipped_only")
def test_main_writes_the_table_of_the_shipped_parts(tmp_path: Path) -> None:
    out = tmp_path / "sweep.json"
    assert main(["--out", str(out), "--source", "shipped", "--parts", "STM32U575QGIx"]) == 0
    doc = json.loads(out.read_text())
    assert doc["format"] == 1
    assert doc["cubemx_db"] is None
    assert [row["part"] for row in doc["parts"]] == ["STM32U575QGIx"]


@pytest.mark.usefixtures("shipped_only")
def test_a_db_part_file_that_does_not_read_is_a_row_field(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    db = tmp_path / "db"
    (db / "mcu").mkdir(parents=True)
    (db / "mcu" / "STM32U575QGIx.xml").write_text("<Mcu")
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(db))
    row = sweep_part("STM32U575QGIx")
    assert row["db_error"].startswith("DescriptionError: ")
    assert row["bonded"] is None
    assert row["description"]["ok"] is True


def test_a_crashing_child_gives_its_row_and_leaves_the_others_intact() -> None:
    rows = sweep(["A", "CRASH", "Z"], child=FAKE, jobs=3)
    assert [row["part"] for row in rows] == ["A", "CRASH", "Z"]
    assert rows[1]["error"] == "crashed"
    assert rows[1]["returncode"] == 3
    assert rows[0] == {"part": "A", "error": None, "running": None}
    assert rows[2] == {"part": "Z", "error": None, "running": None}


def test_a_child_past_the_timeout_gives_a_timeout_row() -> None:
    rows = sweep(["A", "SLOW"], child=FAKE, jobs=2, timeout=2.0)
    assert rows == [
        {"part": "A", "error": None, "running": None},
        {"part": "SLOW", "error": "timeout"},
    ]


def test_rows_are_sorted_by_part_whatever_the_completion_order(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    parts = ["P0", "P1", "P2", "P3"]
    for index, part in enumerate(parts):
        monkeypatch.setenv(f"SWEEP_DELAY_{part}", str((3 - index) * 0.3))
    done: list[str] = []
    rows = sweep(list(reversed(parts)), child=FAKE, jobs=4, on_row=lambda r: done.append(r["part"]))
    assert sorted(done) == parts
    assert [row["part"] for row in rows] == parts


def test_at_most_jobs_children_run_at_once(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    parts = [f"P{index}" for index in range(8)]
    monkeypatch.setenv("SWEEP_MARKS", str(tmp_path))
    for part in parts:
        monkeypatch.setenv(f"SWEEP_DELAY_{part}", "0.4")
    rows = sweep(parts, child=FAKE, jobs=2)
    assert len(rows) == len(parts)
    assert max(row["running"] for row in rows) == 2
    assert list(tmp_path.iterdir()) == []


def test_fewer_than_one_job_is_refused() -> None:
    with pytest.raises(ValueError, match="at least 1"):
        sweep(["A"], child=FAKE, jobs=0)


def test_db_parts_lists_stm32_part_files_only(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    mcu_dir = tmp_path / "mcu"
    mcu_dir.mkdir()
    for name in ("families", "rules", "compatibility", "STM32X0(4-6)Px", "STM32Y1Tx"):
        (mcu_dir / f"{name}.xml").write_text("<Mcu/>")
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(tmp_path))
    assert db_parts([]) == ["STM32X0(4-6)Px", "STM32Y1Tx"]
    assert db_parts(["STM32X*"]) == ["STM32X0(4-6)Px"]


def test_a_range_stem_is_one_refname_with_its_db_facts() -> None:
    name = "STM32C011F(4-6)Px"
    if not (default_search_paths().cubemx_db / "mcu" / f"{name}.xml").is_file():
        pytest.skip(f"no CubeMX db file for {name}")
    row = sweep_part(name)
    assert row["part"] == name
    assert row["package"]
    assert row["db_error"] is None
    assert f"{name}: " in row["description"]["error"]


@pytest.mark.usefixtures("shipped_only")
def test_a_local_sweep_without_a_db_stops_naming_the_path(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    with pytest.raises(SystemExit) as exc:
        main(["--out", str(tmp_path / "sweep.json")])
    assert exc.value.code == 2
    assert str(tmp_path / "absent" / "mcu") in capsys.readouterr().err
    assert not (tmp_path / "sweep.json").exists()


def test_a_sweep_matching_no_part_stops(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    (tmp_path / "mcu").mkdir()
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(tmp_path))
    with pytest.raises(SystemExit) as exc:
        main(["--out", str(tmp_path / "sweep.json"), "--parts", "STM32Q*"])
    assert exc.value.code == 2
    assert "no local part matches" in capsys.readouterr().err
