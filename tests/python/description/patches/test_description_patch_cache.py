"""Each patch file is parsed once per process and parsed again once its bytes change."""

import os
from pathlib import Path

import pytest
from popcorn.stm32.description._errors import DescriptionError
from popcorn.stm32.description._patches import load_patches, part_facts, read_patch

_PATCH = """
format = 1
source = "{source}"
[match]
parts = ["TOY1"]
"""

_FACTS = """
format = 1
[match]
parts = ["TOY1"]

[[part_facts]]
part = "TOY1"
ram_kib = {ram}
source = "probe"
"""


def _rewrite_keeping_size_and_mtime(path: Path, text: str) -> None:
    info = path.stat()
    assert len(text.encode()) == info.st_size
    path.write_text(text, encoding="utf-8")
    os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns))


def test_an_edited_patch_file_is_seen_at_the_next_load(tmp_path: Path) -> None:
    path = tmp_path / "patch.toml"
    path.write_text(_PATCH.format(source="AAAA"), encoding="utf-8")
    [first] = load_patches([path], part="TOY1", svd_name="x.svd")
    assert first.patch.source == "AAAA"
    _rewrite_keeping_size_and_mtime(path, _PATCH.format(source="BBBB"))
    [second] = load_patches([path], part="TOY1", svd_name="x.svd")
    assert second.patch.source == "BBBB"


def test_an_unchanged_patch_file_is_parsed_once(tmp_path: Path) -> None:
    path = tmp_path / "patch.toml"
    path.write_text(_PATCH.format(source="AAAA"), encoding="utf-8")
    first = read_patch(path)
    assert read_patch(path) is first
    [loaded] = load_patches([path], part="TOY1", svd_name="x.svd")
    assert loaded.patch is first


def test_an_edited_part_facts_row_is_seen_at_the_next_call(tmp_path: Path) -> None:
    path = tmp_path / "facts.toml"
    path.write_text(_FACTS.format(ram=4), encoding="utf-8")
    assert part_facts([path])["TOY1"].ram_kib == 4
    _rewrite_keeping_size_and_mtime(path, _FACTS.format(ram=8))
    assert part_facts([path])["TOY1"].ram_kib == 8


def test_a_refused_patch_file_is_refused_again_until_it_is_fixed(tmp_path: Path) -> None:
    path = tmp_path / "patch.toml"
    path.write_text("format = \n", encoding="utf-8")
    for _ in range(2):
        with pytest.raises(DescriptionError) as excinfo:
            read_patch(path)
        assert excinfo.value.key == "<root>"
    path.write_text(_PATCH.format(source="AAAA").replace("source", "sauce", 1), encoding="utf-8")
    for _ in range(2):
        with pytest.raises(DescriptionError):
            read_patch(path)
    path.write_text(_PATCH.format(source="AAAA"), encoding="utf-8")
    assert read_patch(path).source == "AAAA"


def test_a_refused_part_facts_row_is_refused_again_until_it_is_fixed(tmp_path: Path) -> None:
    path = tmp_path / "facts.toml"
    path.write_text(_FACTS.format(ram=4).replace('part = "TOY1"', 'part = "TOY2"'), "utf-8")
    for _ in range(2):
        with pytest.raises(DescriptionError) as excinfo:
            part_facts([path])
        assert excinfo.value.key == "part_facts[0].part"
    path.write_text(_FACTS.format(ram=4), encoding="utf-8")
    assert part_facts([path])["TOY1"].ram_kib == 4
