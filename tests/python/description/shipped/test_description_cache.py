import json
from dataclasses import replace
from pathlib import Path

import pytest
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._cache import cache_key, cached_description
from popcorn.stm32.description._model import PartDescription
from popcorn.stm32.description._sources import Sources, compute_source_digests, generator_string


def test_cache_miss_then_hit(
    u575_sources: Sources, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    calls = 0

    def counting_build(sources: Sources) -> PartDescription:
        nonlocal calls
        calls += 1
        return build_description(sources)

    monkeypatch.setattr("popcorn.stm32.description._cache.build_description", counting_build)

    path = cached_description("STM32U575QGIx", cache_dir=tmp_path)
    assert path.is_file()
    assert calls == 1

    path_again = cached_description("STM32U575QGIx", cache_dir=tmp_path)
    assert path_again == path
    assert calls == 1


def test_cache_key_changes_with_patch_byte(u575_sources: Sources, tmp_path: Path) -> None:
    patched_patch_dir = tmp_path / "patches"
    patched_patch_dir.mkdir()
    for patch in u575_sources.patches:
        (patched_patch_dir / patch.name).write_bytes(patch.read_bytes())
    tweaked = patched_patch_dir / u575_sources.patches[0].name
    tweaked.write_text(tweaked.read_text(encoding="utf-8") + "\n", encoding="utf-8")

    tweaked_sources = replace(
        u575_sources, patches=[patched_patch_dir / p.name for p in u575_sources.patches]
    )

    generator = generator_string()
    key_a = cache_key(u575_sources.part, None, generator, compute_source_digests(u575_sources))
    key_b = cache_key(
        tweaked_sources.part, None, generator, compute_source_digests(tweaked_sources)
    )
    assert key_a != key_b


def test_cache_key_changes_with_svd_byte(u575_sources: Sources, tmp_path: Path) -> None:
    tweaked_svd = tmp_path / "tweaked.svd"
    tweaked_svd.write_bytes(u575_sources.svd.read_bytes() + b"\n")
    tweaked_sources = replace(u575_sources, svd=tweaked_svd)

    generator = generator_string()
    key_a = cache_key(u575_sources.part, None, generator, compute_source_digests(u575_sources))
    key_b = cache_key(
        tweaked_sources.part, None, generator, compute_source_digests(tweaked_sources)
    )
    assert key_a != key_b


def test_xdg_cache_home_honoured(
    u575_sources: Sources, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    xdg = tmp_path / "xdg"
    monkeypatch.setenv("XDG_CACHE_HOME", str(xdg))

    path = cached_description("STM32U575QGIx")
    assert path.is_file()
    assert xdg in path.parents
    data = json.loads(path.read_text(encoding="utf-8"))
    assert data["part"] == "STM32U575QGIx"
