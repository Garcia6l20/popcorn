import json
from dataclasses import replace
from pathlib import Path

import pytest
from popcorn import MissingInput
from popcorn.stm32.description._builder import build_description
from popcorn.stm32.description._cache import cache_key, cached_description
from popcorn.stm32.description._errors import DescriptionError, MissingSource
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


def test_a_local_build_with_no_cmsis_header_raises_missing_source_naming_the_directories(
    u575_sources: Sources, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    empty = tmp_path / "no_headers"
    empty.mkdir()
    cache = tmp_path / "cache"
    monkeypatch.setenv("POPCORN_CMSIS_DEVICE_DIRS", str(empty))
    with pytest.raises(MissingSource) as caught:
        cached_description("STM32U575QGIx", cache_dir=cache)
    error = caught.value
    assert isinstance(error, MissingInput)
    assert isinstance(error, DescriptionError)
    assert error.key == "cmsis_header"
    assert error.path == empty
    assert "stm32u575xx.h" in error.expected
    assert str(empty) in error.expected
    assert not cache.exists()


def test_a_fixture_skipping_on_a_description_error_skips_on_a_missing_cmsis_header(
    u575_sources: Sources, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("POPCORN_CMSIS_DEVICE_DIRS", str(tmp_path))

    def fixture() -> Path:
        try:
            return cached_description("STM32U575QGIx", cache_dir=tmp_path / "cache")
        except DescriptionError as error:
            pytest.skip(f"no STM32U575 description: {error}")

    with pytest.raises(pytest.skip.Exception):
        fixture()


def test_a_missing_cmsis_header_names_every_directory_tried(
    u575_sources: Sources, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    first, second = tmp_path / "first", tmp_path / "second"
    first.mkdir()
    second.mkdir()
    monkeypatch.setenv("POPCORN_CMSIS_DEVICE_DIRS", f"{first}:{second}")
    with pytest.raises(MissingSource) as caught:
        cached_description("STM32U575QGIx", cache_dir=tmp_path / "cache")
    assert f"{first}, {second}" in caught.value.expected


def test_a_missing_cmsis_header_with_no_directory_to_search_names_the_repository(
    u575_sources: Sources, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    home = tmp_path / "home"
    home.mkdir()
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.delenv("POPCORN_CMSIS_DEVICE_DIRS")
    with pytest.raises(MissingSource) as caught:
        cached_description(
            "STM32U575QGIx",
            cache_dir=tmp_path / "cache",
            cubemx_db=u575_sources.mcu_db.parents[1],
            svd=u575_sources.svd,
        )
    error = caught.value
    assert error.key == "cmsis_header"
    assert error.path == home / "STM32Cube" / "Repository"
    assert f"{error.path} holds no STM32Cube_FW_<family>_V*" in error.expected
    assert "POPCORN_CMSIS_DEVICE_DIRS" in error.expected


def test_a_cmsis_header_given_by_argument_builds_without_the_search(
    u575_sources: Sources, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    assert u575_sources.cmsis_header is not None
    empty = tmp_path / "no_headers"
    empty.mkdir()
    monkeypatch.setenv("POPCORN_CMSIS_DEVICE_DIRS", str(empty))
    path = cached_description(
        "STM32U575QGIx", cache_dir=tmp_path / "cache", cmsis_header=u575_sources.cmsis_header
    )
    assert path.is_file()


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


def test_the_cache_key_and_the_dump_carry_the_models_schema_version() -> None:
    import hashlib
    from dataclasses import fields

    from popcorn.stm32.description import _binder

    default = next(f.default for f in fields(PartDescription) if f.name == "schema_version")
    assert default == _binder.SCHEMA_VERSION
    payload = {
        "part": "P",
        "core": None,
        "schema_version": default,
        "generator": "g",
        "sources": {"a": "b"},
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    assert cache_key("P", None, "g", {"a": "b"}) == hashlib.sha256(canonical.encode()).hexdigest()
