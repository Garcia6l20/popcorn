"""prune_stage: staged package files whose source is gone are removed when the build script
runs."""

from pathlib import Path

from popcorn_build.python import prune_stage


def _write(root: Path, *relatives: str) -> None:
    for relative in relatives:
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(relative)


def test_a_staged_file_without_a_source_is_removed_and_the_others_stay(tmp_path: Path) -> None:
    source, stage = tmp_path / "python" / "popcorn", tmp_path / "build" / "popcorn"
    kept = ("__init__.py", "stm32/data/STM32U5xx.blocks.json.xz", "stm32/x.toml", "py.typed")
    _write(source, *kept)
    stale = ("stm32/data/STM32U575QGIx.json.xz", "old.py", "stm32/description/lines.toml")
    _write(stage, *kept, *stale, "arm/_regs.py", "_core.so", "_core.pyi")
    removed = prune_stage(stage, source, keep={"arm/_regs.py"})
    assert sorted(path.relative_to(stage).as_posix() for path in removed) == sorted(stale)
    remaining = sorted(path.relative_to(stage).as_posix() for path in stage.rglob("*.*"))
    assert remaining == sorted([*kept, "arm/_regs.py", "_core.so", "_core.pyi"])


def test_a_missing_stage_prunes_nothing(tmp_path: Path) -> None:
    source = tmp_path / "python" / "popcorn"
    _write(source, "__init__.py")
    assert prune_stage(tmp_path / "absent", source) == []


def test_a_removed_subpackage_leaves_no_directory_and_live_caches_stay(tmp_path: Path) -> None:
    source, stage = tmp_path / "python" / "popcorn", tmp_path / "build" / "popcorn"
    _write(source, "__init__.py", "y/__init__.py")
    _write(
        stage,
        "__init__.py",
        "y/__init__.py",
        "y/__pycache__/__init__.cpython-314.pyc",
        "x/__init__.py",
        "x/__pycache__/__init__.cpython-314.pyc",
        "x/z/__pycache__/m.cpython-314.pyc",
    )
    prune_stage(stage, source)
    assert not (stage / "x").exists()
    assert (stage / "y" / "__pycache__" / "__init__.cpython-314.pyc").exists()
