"""Shipped layers and the lookup order of description_path, with no ST file reachable."""

import hashlib
import json
import lzma
import os
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any, cast

import popcorn
import pytest
from popcorn.stm32.description import __main__ as cli
from popcorn.stm32.description import _layers, _shipped
from popcorn.stm32.description._errors import DescriptionError, MissingSource
from popcorn.stm32.description._layers import (
    choose_reference,
    dump,
    factor,
    layer_family,
    pack,
)
from popcorn.stm32.description._shipped import (
    SHIPPED_DIR,
    BuiltPart,
    compose_shipped,
    description_path,
    shipped_description,
    shipped_names,
)

from .conftest import write_layers

M33_JSON = Path(__file__).parents[3] / "tests" / "data" / "core" / "m33.json"
NAME = "STM32T001ABCx"
PARTS = "STM32T001xx.parts.json.xz"
LINE = "STM32T001xx.line.json.xz"
FAMILY = "STM32T0xx.blocks.json.xz"


def _description(part: str = NAME, generator: str = "hand-written") -> dict[str, Any]:
    raw = cast(dict[str, Any], json.loads(M33_JSON.read_text()))
    return raw | {"part": part, "generator": generator}


def _description_text(part: str = NAME, generator: str = "hand-written") -> str:
    return dump(_description(part, generator))


def _decode(directory: Path, file: str) -> dict[str, Any]:
    return cast(dict[str, Any], json.loads(lzma.decompress((directory / file).read_bytes())))


def _rewrite(directory: Path, file: str, layer: dict[str, Any]) -> None:
    (directory / file).write_bytes(pack(layer))
    if file == LINE:
        parts = _decode(directory, PARTS)
        parts["line_sha256"] = hashlib.sha256(dump(layer).encode()).hexdigest()
        (directory / PARTS).write_bytes(pack(parts))


@pytest.fixture
def shipped_dir(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    directory = tmp_path / "data"
    write_layers(directory, {NAME: _description()})
    monkeypatch.setattr(_shipped, "SHIPPED_DIR", directory)
    absent = tmp_path / "absent"
    monkeypatch.setenv("HOME", str(absent))
    monkeypatch.setenv("POPCORN_CUBEMX_DB", str(absent / "db"))
    monkeypatch.setenv("POPCORN_STM32_SVD_DIRS", str(absent / "svd"))
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    monkeypatch.delenv("POPCORN_STM32_DESCRIPTION_DIRS", raising=False)
    monkeypatch.delenv("POPCORN_STM32_BUILD", raising=False)
    return directory


@pytest.fixture
def compose_calls(monkeypatch: pytest.MonkeyPatch) -> list[str]:
    calls: list[str] = []
    real: Callable[..., dict[str, Any]] = _layers.compose

    def counting(name: str, *args: Any, **kwargs: Any) -> dict[str, Any]:
        calls.append(name)
        return real(name, *args, **kwargs)

    monkeypatch.setattr(_shipped, "compose", counting)
    return calls


def _cached(tmp_path: Path) -> list[Path]:
    return sorted((tmp_path / "cache" / "popcorn" / "mcu" / "shipped").glob("*.json"))


def test_a_shipped_part_resolves_with_no_st_files(shipped_dir: Path, tmp_path: Path) -> None:
    exact = description_path(NAME)
    assert exact.parent == tmp_path / "cache" / "popcorn" / "mcu" / "shipped"
    assert exact.read_text() == _description_text()
    assert popcorn.load_description(exact).part == NAME
    assert description_path("STM32T001ABC6") == exact


def test_a_second_lookup_reuses_the_composed_file(
    shipped_dir: Path, compose_calls: list[str]
) -> None:
    first = description_path(NAME)
    assert description_path(NAME) == first
    assert compose_calls == [NAME]


@pytest.mark.parametrize("file", [PARTS, LINE, FAMILY])
def test_other_bytes_of_any_layer_file_give_a_new_cache_name(
    shipped_dir: Path, tmp_path: Path, file: str
) -> None:
    first = description_path(NAME)
    raw = lzma.decompress((shipped_dir / file).read_bytes())
    (shipped_dir / file).write_bytes(lzma.compress(raw, format=lzma.FORMAT_XZ, preset=0))
    second = description_path(NAME)
    assert second != first
    assert second.read_bytes() == first.read_bytes()


def _age(path: Path, seconds: float) -> None:
    then = time.time() - seconds
    os.utime(path, (then, then))


def test_a_changed_shipped_layer_removes_versions_unused_for_a_week(
    shipped_dir: Path, tmp_path: Path
) -> None:
    versions = [factor({NAME: _description(generator=g)}, {"STM32T001xx": NAME}) for g in "ab"]
    paths: set[Path] = set()
    for index in range(1000):
        for cached in _cached(tmp_path):
            _age(cached, _shipped.STALE_SECONDS + 86400)
        for file, data in versions[index % 2].items():
            (shipped_dir / file).write_bytes(data)
        path = description_path(NAME)
        paths.add(path)
        assert _cached(tmp_path) == [path]
    assert len(paths) == 2


def test_two_shipped_versions_keep_their_files_in_a_shared_cache(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    other_dir = tmp_path / "other-data"
    write_layers(other_dir, {NAME: _description(generator="other")})
    first = description_path(NAME)
    _age(first, _shipped.STALE_SECONDS - 86400)
    monkeypatch.setattr(_shipped, "SHIPPED_DIR", other_dir)
    second = description_path(NAME)
    assert second != first
    assert first.is_file()
    assert _cached(tmp_path) == sorted([first, second])
    monkeypatch.setattr(_shipped, "SHIPPED_DIR", shipped_dir)
    assert description_path(NAME) == first
    assert first.stat().st_mtime > time.time() - 60


def test_a_stale_file_another_user_owns_stays_and_the_lookup_succeeds(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    stale = description_path(NAME)
    _age(stale, _shipped.STALE_SECONDS + 86400)
    write_layers(shipped_dir, {NAME: _description(generator="other")})
    real = Path.unlink

    def sticky(self: Path, missing_ok: bool = False) -> None:
        if self == stale:
            raise PermissionError(1, "Operation not permitted", str(self))
        real(self, missing_ok=missing_ok)

    monkeypatch.setattr(Path, "unlink", sticky)
    fresh = description_path(NAME)
    assert fresh != stale
    assert _cached(tmp_path) == sorted([stale, fresh])


def test_a_failed_write_leaves_no_temporary_file(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def refuse(src: object, dst: object) -> None:
        raise OSError("disk full")

    monkeypatch.setattr("popcorn.stm32.description._shipped.os.replace", refuse)
    with pytest.raises(OSError, match="disk full"):
        description_path(NAME)
    assert list((tmp_path / "cache" / "popcorn" / "mcu" / "shipped").iterdir()) == []


def test_a_core_suffixed_shipped_part_answers_only_for_its_core(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    directory = tmp_path / "core-data"
    write_layers(directory, {f"{NAME}-CM4": _description()})
    monkeypatch.setattr(_shipped, "SHIPPED_DIR", directory)
    assert shipped_names() == [f"{NAME}-CM4"]
    assert shipped_description(NAME) is None
    assert shipped_description(NAME, core="CM7") is None
    found = shipped_description(NAME, core="CM4")
    assert found is not None
    assert found.name.startswith(f"{NAME}-CM4-")
    assert found.read_text() == _description_text()


def test_only_parts_layers_name_shipped_parts(shipped_dir: Path) -> None:
    (shipped_dir / "STM32T002xx.line.json.xz").write_bytes((shipped_dir / LINE).read_bytes())
    (shipped_dir / "STM32T1xx.blocks.json.xz").write_bytes((shipped_dir / FAMILY).read_bytes())
    (shipped_dir / "STM32T002ABCx.json.xz").write_bytes(lzma.compress(b"{}"))
    assert shipped_names() == [NAME]


def test_parts_of_one_line_share_its_layers(shipped_dir: Path) -> None:
    other = "STM32T001DEFx"
    other_description = _description(other)
    other_description["irqs"] = other_description["irqs"][:-1]
    files = write_layers(shipped_dir, {NAME: _description(), other: other_description})
    assert sorted(files) == sorted([PARTS, LINE, FAMILY])
    deltas = _decode(shipped_dir, PARTS)["parts"]
    assert [key for key in ("schema_version", "core", "irqs") if key in deltas[other]] == ["irqs"]
    assert shipped_names() == [NAME, other]
    assert compose_shipped(other) == dump(other_description)
    assert compose_shipped(NAME) == _description_text()


def test_a_description_directory_wins_over_the_shipped_layers(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    first = tmp_path / "first"
    second = tmp_path / "second"
    for directory in (first, second):
        directory.mkdir()
        (directory / f"{NAME}.json").write_text(_description_text(generator=directory.name))
    monkeypatch.setenv(
        "POPCORN_STM32_DESCRIPTION_DIRS", f"{tmp_path / 'absent-dir'}:{first}:{second}"
    )
    assert description_path("STM32T001ABC6") == first / f"{NAME}.json"
    assert _cached(tmp_path) == []


@pytest.mark.parametrize("argument", ["svd", "cubemx_db", "cmsis_header"])
def test_a_local_input_skips_the_shipped_layers(
    shipped_dir: Path, tmp_path: Path, argument: str
) -> None:
    absent = tmp_path / f"absent-{argument}"
    with pytest.raises(MissingSource) as excinfo:
        if argument == "svd":
            description_path(NAME, svd=absent)
        elif argument == "cubemx_db":
            description_path(NAME, cubemx_db=absent)
        else:
            description_path(NAME, cmsis_header=absent)
    assert "shipped:" not in excinfo.value.expected
    assert _cached(tmp_path) == []


def test_a_local_build_request_skips_the_shipped_layers(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("POPCORN_STM32_BUILD", "local")
    with pytest.raises(MissingSource):
        description_path(NAME)
    assert _cached(tmp_path) == []


def test_an_unknown_build_request_is_refused_before_any_lookup(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    directory = tmp_path / "dir"
    directory.mkdir()
    (directory / f"{NAME}.json").write_text(_description_text())
    monkeypatch.setenv("POPCORN_STM32_DESCRIPTION_DIRS", str(directory))
    monkeypatch.setenv("POPCORN_STM32_BUILD", "shipped")
    with pytest.raises(DescriptionError) as excinfo:
        description_path(NAME)
    assert excinfo.value.key == "POPCORN_STM32_BUILD"
    assert excinfo.value.expected == "unset or local, not 'shipped'"


def test_an_unshipped_part_without_st_files_names_the_shipped_parts(shipped_dir: Path) -> None:
    with pytest.raises(MissingSource) as excinfo:
        description_path("STM32T002ABCx")
    assert f"a shipped description, shipped: {NAME}, or ST's files" in excinfo.value.expected


def test_a_lookup_with_nothing_shipped_says_so(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    empty = tmp_path / "empty-data"
    empty.mkdir()
    monkeypatch.setattr(_shipped, "SHIPPED_DIR", empty)
    with pytest.raises(MissingSource) as excinfo:
        description_path(NAME)
    assert "a shipped description, shipped: none, or ST's files" in excinfo.value.expected


def _refused(tmp_path: Path) -> DescriptionError:
    with pytest.raises(DescriptionError) as excinfo:
        description_path(NAME)
    assert _cached(tmp_path) == []
    return excinfo.value


@pytest.mark.parametrize("file", [PARTS, LINE, FAMILY])
def test_a_layer_file_past_the_cap_is_refused(shipped_dir: Path, tmp_path: Path, file: str) -> None:
    (shipped_dir / file).write_bytes(
        lzma.compress(bytes(_shipped.MAX_SHIPPED_BYTES + (1 << 20)), preset=0)
    )
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / file)
    assert error.expected == f"at most {_shipped.MAX_SHIPPED_BYTES} bytes decompressed"


@pytest.mark.parametrize("file", [PARTS, LINE, FAMILY])
@pytest.mark.parametrize(
    ("data", "expected"),
    [
        (b"this is not an xz stream at all", "an xz stream"),
        (lzma.compress(b"{}")[:-8], "a complete xz stream"),
        (lzma.compress(b"{}") + b"tail", "one xz stream and nothing after it"),
    ],
)
def test_a_broken_layer_file_is_refused(
    shipped_dir: Path, tmp_path: Path, file: str, data: bytes, expected: str
) -> None:
    (shipped_dir / file).write_bytes(data)
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / file)
    assert error.expected == expected


@pytest.mark.parametrize("file", [PARTS, LINE, FAMILY])
def test_a_layer_of_another_format_is_refused(shipped_dir: Path, tmp_path: Path, file: str) -> None:
    layer = _decode(shipped_dir, file)
    _rewrite(shipped_dir, file, layer | {"format": 2})
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / file)
    assert (error.key, error.expected) == ("format", "layer format 1")


@pytest.mark.parametrize("file", [LINE, FAMILY])
def test_a_missing_layer_file_is_refused(shipped_dir: Path, tmp_path: Path, file: str) -> None:
    (shipped_dir / file).unlink()
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / file)


@pytest.mark.parametrize("file", [PARTS, LINE, FAMILY])
def test_a_layer_nested_too_deep_for_the_json_parser_is_refused_naming_it(
    shipped_dir: Path, tmp_path: Path, file: str
) -> None:
    (shipped_dir / file).write_bytes(lzma.compress(b"[" * 200000, format=lzma.FORMAT_XZ))
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / file)
    assert (error.key, error.expected) == ("<root>", "valid JSON")


@pytest.mark.parametrize("file", [PARTS, LINE, FAMILY])
def test_a_layer_nested_past_the_depth_cap_is_refused_naming_it(
    shipped_dir: Path, tmp_path: Path, file: str
) -> None:
    depth = _shipped.MAX_LAYER_DEPTH
    (shipped_dir / file).write_bytes(pack(cast(Any, _nested(depth))))
    assert _refused(tmp_path).expected != f"JSON nested at most {depth} deep"
    (shipped_dir / file).write_bytes(lzma.compress(b"[" * (depth + 1) + b"]" * (depth + 1)))
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / file)
    assert (error.key, error.expected) == ("<root>", f"JSON nested at most {depth} deep")


def test_a_parts_layer_nested_deeper_than_the_json_encoder_takes_is_refused_naming_it(
    shipped_dir: Path, tmp_path: Path
) -> None:
    layer = _decode(shipped_dir, PARTS)
    layer["parts"][NAME]["regions"] = "@deep@"
    text = json.dumps(layer).replace('"@deep@"', "[" * 50000 + "]" * 50000)
    (shipped_dir / PARTS).write_bytes(lzma.compress(text.encode("ascii")))
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / PARTS)
    assert error.expected == f"JSON nested at most {_shipped.MAX_LAYER_DEPTH} deep"


def _nested(depth: int) -> list[object]:
    value: list[object] = []
    for _ in range(depth - 1):
        value = [value]
    return value


def test_a_parts_layer_of_another_line_version_is_refused(
    shipped_dir: Path, tmp_path: Path
) -> None:
    line = _decode(shipped_dir, LINE)
    line["irqs"] = line["irqs"][:-1]
    (shipped_dir / LINE).write_bytes(pack(line))
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / PARTS)
    assert error.key == "line_sha256"


def test_a_missing_block_is_refused(shipped_dir: Path, tmp_path: Path) -> None:
    family = _decode(shipped_dir, FAMILY)
    line = _decode(shipped_dir, LINE)
    key = line["peripherals"][0]["block"]
    del family["blocks"][key]
    (shipped_dir / FAMILY).write_bytes(pack(family))
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / FAMILY)
    assert (error.key, error.expected) == (f"blocks.{key}", "the block of a header")


def test_a_tampered_block_is_refused(shipped_dir: Path, tmp_path: Path) -> None:
    family = _decode(shipped_dir, FAMILY)
    line = _decode(shipped_dir, LINE)
    key = line["peripherals"][0]["block"]
    family["blocks"][key]["registers"][0]["reset"] ^= 1
    (shipped_dir / FAMILY).write_bytes(pack(family))
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / FAMILY)
    assert (error.key, error.expected) == (f"blocks.{key}", "a block hashing to key")


def test_a_delta_naming_an_instance_the_line_lacks_is_refused(
    shipped_dir: Path, tmp_path: Path
) -> None:
    parts = _decode(shipped_dir, PARTS)
    line = _decode(shipped_dir, LINE)
    parts["parts"][NAME]["peripherals"]["NOSUCH"] = line["peripherals"][0] | {"name": "NOSUCH"}
    (shipped_dir / PARTS).write_bytes(pack(parts))
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / PARTS)
    assert error.key == f"parts.{NAME}.peripherals.NOSUCH"


def test_a_composition_over_the_cap_is_refused(
    shipped_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    largest = max(
        len(lzma.decompress((shipped_dir / f).read_bytes())) for f in (PARTS, LINE, FAMILY)
    )
    assert len(_description_text()) > largest
    monkeypatch.setattr(_shipped, "MAX_SHIPPED_BYTES", largest)
    error = _refused(tmp_path)
    assert error.file == str(shipped_dir / PARTS)
    assert error.expected == f"a composed description of at most {largest} bytes"


@pytest.mark.timeout(10)
def test_one_large_block_referenced_by_every_instance_is_refused_before_composing(
    shipped_dir: Path, tmp_path: Path
) -> None:
    block = {"registers": [{"name": "R", "pad": "x" * (1 << 20)}], "cross_rules": []}
    key = _layers.block_key(block)
    headers = [{"name": f"P{index}", "block": key} for index in range(100)]
    line = _decode(shipped_dir, LINE) | {"peripherals": headers}
    family = _decode(shipped_dir, FAMILY) | {"blocks": {key: block}}
    parts = _decode(shipped_dir, PARTS)
    parts["parts"][NAME]["peripherals"] = {}
    (shipped_dir / PARTS).write_bytes(pack(parts))
    (shipped_dir / FAMILY).write_bytes(pack(family))
    _rewrite(shipped_dir, LINE, line)
    assert _shipped.MAX_SHIPPED_BYTES == 64 << 20
    start = time.perf_counter()
    error = _refused(tmp_path)
    assert time.perf_counter() - start < 1.0
    assert error.file == str(shipped_dir / PARTS)
    assert error.expected == f"a composed description of at most {64 << 20} bytes"


def test_a_part_with_its_own_block_composes_back_byte_equal(tmp_path: Path) -> None:
    reference = cast(dict[str, Any], json.loads(compose_shipped("STM32U575QGIx")))
    other = json.loads(dump(reference))
    other["part"] = "STM32U575CGTx"
    for peripheral in other["peripherals"]:
        if peripheral["name"] in ("UART5", "SEC_UART5"):
            assert len(peripheral["cross_rules"]) == 13
            peripheral["cross_rules"] = []
    descriptions = {"STM32U575QGIx": reference, "STM32U575CGTx": other}
    directory = tmp_path / "data"
    write_layers(directory, descriptions, {"STM32U575xx": "STM32U575QGIx"})
    parts = _decode(directory, "STM32U575xx.parts.json.xz")
    line = _decode(directory, "STM32U575xx.line.json.xz")
    delta = parts["parts"]["STM32U575CGTx"]["peripherals"]
    assert sorted(delta) == ["SEC_UART5", "UART5"]
    line_blocks = {h["name"]: h["block"] for h in line["peripherals"]}
    assert all(delta[name]["block"] != line_blocks[name] for name in delta)
    assert parts["parts"]["STM32U575QGIx"]["peripherals"] == {}
    for name, description in descriptions.items():
        text = compose_shipped(name, directory)
        same = text == dump(description)
        assert same, name
        assert popcorn.description_digest(
            popcorn.parse_description(text, name)
        ) == popcorn.description_digest(popcorn.parse_description(dump(description), name))


def test_factor_refuses_a_part_whose_instances_differ_from_its_reference() -> None:
    other = _description("STM32T001DEFx")
    other["peripherals"] = other["peripherals"][::-1]
    with pytest.raises(DescriptionError) as excinfo:
        factor({NAME: _description(), "STM32T001DEFx": other}, {"STM32T001xx": NAME})
    assert excinfo.value.file == "STM32T001DEFx"


def test_factor_refuses_a_description_with_keys_out_of_order() -> None:
    description = _description()
    description["part"] = description.pop("part")
    with pytest.raises(DescriptionError) as excinfo:
        factor({NAME: description}, {"STM32T001xx": NAME})
    assert excinfo.value.key == "<root>"


def test_a_read_only_cache_still_serves_its_file(
    shipped_dir: Path, monkeypatch: pytest.MonkeyPatch, compose_calls: list[str]
) -> None:
    first = description_path(NAME)

    def read_only(path: object, times: object = None) -> None:
        raise PermissionError("read-only cache")

    monkeypatch.setattr("popcorn.stm32.description._shipped.os.utime", read_only)
    assert description_path(NAME) == first
    assert compose_calls == [NAME]


def test_the_shipped_layers_are_what_factor_writes_for_the_shipped_parts() -> None:
    names = shipped_names()
    assert names
    descriptions = {name: json.loads(compose_shipped(name)) for name in names}
    references: dict[str, str] = {}
    for path in SHIPPED_DIR.glob("*.line.json.xz"):
        line = cast(dict[str, Any], json.loads(lzma.decompress(path.read_bytes())))
        references[line["line"]] = line["reference"]
    files = factor(descriptions, references)
    assert sorted(files) == sorted(path.name for path in SHIPPED_DIR.glob("*.json.xz"))
    for file, data in files.items():
        assert lzma.decompress((SHIPPED_DIR / file).read_bytes()) == lzma.decompress(data), file


@pytest.mark.parametrize("name", shipped_names())
def test_every_alias_of_a_shipped_description_equals_its_base_register_by_register(
    name: str,
) -> None:
    data = cast(dict[str, object], json.loads(compose_shipped(name)))
    peripherals = cast(list[dict[str, object]], data["peripherals"])
    by_name = {cast(str, p["name"]): p for p in peripherals}
    aliases = [p for p in peripherals if p.get("alias_of")]
    assert aliases
    for alias in aliases:
        base = by_name[cast(str, alias["alias_of"])]
        assert alias["registers"] == base["registers"], alias["name"]


SIBLING = "STM32T001ABDx"
OTHER = "STM32T101ABCx"
BONDED = {NAME: 80, SIBLING: 90, OTHER: 10}
"""Bonded pins of the toy parts, SIBLING the reference of their line."""


class ShipEnv:
    def __init__(self, root: Path) -> None:
        self.goldens = root / "golden"
        self.out = root / "out"
        self.built: list[str] = []

    def ship(self, *args: str) -> int:
        return cli.main(["ship", *args, "--pin-data", "unused", "--out", str(self.out), "-j", "1"])


@pytest.fixture
def ship_env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> ShipEnv:
    env = ShipEnv(tmp_path)
    env.goldens.mkdir()
    for name in (NAME, SIBLING, OTHER):
        (env.goldens / f"{name}.json").write_text("{}")

    def build(name: str, *, pin_data: Path) -> BuiltPart:
        env.built.append(name)
        text = _description_text(name, generator=f"built {name}")
        return BuiltPart(text=text, bonded=BONDED[name], flash_kib=1024)

    monkeypatch.setattr(cli, "GOLDEN_DIR", env.goldens)
    monkeypatch.setattr(_shipped, "build_part", build)
    return env


def _files(directory: Path) -> dict[str, bytes]:
    return {path.name: path.read_bytes() for path in sorted(directory.iterdir())}


def test_ship_writes_what_factor_writes_for_every_golden_part_and_the_same_bytes_twice(
    ship_env: ShipEnv,
) -> None:
    assert ship_env.ship() == 0
    expected = factor(
        {name: _description(name, f"built {name}") for name in (NAME, SIBLING, OTHER)},
        {"STM32T001xx": SIBLING, "STM32T101xx": OTHER},
    )
    first = _files(ship_env.out)
    assert first == expected
    assert ship_env.ship() == 0
    assert _files(ship_env.out) == first
    assert ship_env.built == sorted([NAME, SIBLING, OTHER]) * 2


def test_ship_of_one_part_rewrites_its_whole_family_and_leaves_other_families(
    ship_env: ShipEnv,
) -> None:
    ship_env.out.mkdir()
    foreign = ship_env.out / "STM32T1xx.blocks.json.xz"
    foreign.write_bytes(b"other family")
    assert ship_env.ship(SIBLING) == 0
    assert ship_env.built == [NAME, SIBLING]
    assert foreign.read_bytes() == b"other family"
    assert sorted(_files(ship_env.out)) == sorted([PARTS, LINE, FAMILY, foreign.name])
    assert shipped_names(ship_env.out) == [NAME, SIBLING]


def test_ship_removes_the_layers_a_deleted_golden_leaves_behind(ship_env: ShipEnv) -> None:
    assert ship_env.ship() == 0
    (ship_env.goldens / f"{SIBLING}.json").unlink()
    assert ship_env.ship(NAME) == 0
    assert shipped_names(ship_env.out) == [NAME, OTHER]
    assert _decode(ship_env.out, LINE)["reference"] == NAME
    (ship_env.goldens / f"{OTHER}.json").unlink()
    assert ship_env.ship() == 0
    assert sorted(_files(ship_env.out)) == sorted([PARTS, LINE, FAMILY])
    (ship_env.goldens / f"{NAME}.json").unlink()
    assert ship_env.ship() == 0
    assert _files(ship_env.out) == {}


def test_ship_refuses_to_write_without_out(
    ship_env: ShipEnv,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    package_data = tmp_path / "package"
    package_data.mkdir()
    monkeypatch.setattr(cli, "SHIPPED_DIR", package_data)
    assert cli.main(["ship", "--pin-data", "unused"]) == 1
    assert "--out is required to write" in capsys.readouterr().err
    assert ship_env.built == []
    assert list(package_data.iterdir()) == []


def test_ship_replaces_blocks_then_lines_then_parts(
    ship_env: ShipEnv, monkeypatch: pytest.MonkeyPatch
) -> None:
    replaced: list[str] = []
    replace = os.replace

    def record(source: Path, target: Path) -> None:
        replaced.append(Path(target).name.split(".", 1)[1])
        replace(source, target)

    monkeypatch.setattr("popcorn.stm32.description._shipped.os.replace", record)
    assert ship_env.ship() == 0
    kinds = ["blocks.json.xz"] * 2 + ["line.json.xz"] * 2 + ["parts.json.xz"] * 2
    assert replaced == kinds


def test_ship_removes_temporary_files_an_earlier_run_left_in_its_families(
    ship_env: ShipEnv,
) -> None:
    ship_env.out.mkdir()
    left = ship_env.out / f"{PARTS}.tmp.999999"
    foreign = ship_env.out / "STM32T1xx.blocks.json.xz.tmp.999999"
    left.write_bytes(b"killed")
    foreign.write_bytes(b"killed")
    assert ship_env.ship(NAME) == 0
    assert not left.exists()
    assert foreign.exists()


def test_ship_leaves_no_temporary_file_when_a_write_fails(
    ship_env: ShipEnv, monkeypatch: pytest.MonkeyPatch
) -> None:
    def refuse(source: object, target: object) -> None:
        raise OSError("disk full")

    monkeypatch.setattr("popcorn.stm32.description._shipped.os.replace", refuse)
    with pytest.raises(OSError, match="disk full"):
        ship_env.ship()
    assert _files(ship_env.out) == {}


@pytest.mark.parametrize(
    ("args", "message"),
    [
        (["--core", "cm33"], "--core needs a part list"),
        (["STM32T001ABEx"], "STM32T001ABEx: a golden file"),
        ([NAME, "--core", "cm33"], "a golden file of the part core cm33"),
    ],
)
def test_ship_refuses_parts_without_a_golden_and_a_core_without_parts(
    ship_env: ShipEnv, capsys: pytest.CaptureFixture[str], args: list[str], message: str
) -> None:
    assert ship_env.ship(*args) == 1
    assert message in capsys.readouterr().err
    assert ship_env.built == []


def test_ship_check_compares_the_composed_digest_with_a_fresh_build(
    ship_env: ShipEnv, capsys: pytest.CaptureFixture[str]
) -> None:
    assert ship_env.ship() == 0
    capsys.readouterr()
    assert ship_env.ship("--check") == 0
    assert len(capsys.readouterr().out.splitlines()) == 3
    line = _decode(ship_env.out, LINE)
    line["peripherals"][0]["base"] += 0x1000
    _rewrite(ship_env.out, LINE, line)
    assert ship_env.ship(NAME, "--check") == 1
    assert f"{NAME} shipped " in capsys.readouterr().err


def test_ship_check_refuses_a_tampered_block(
    ship_env: ShipEnv, capsys: pytest.CaptureFixture[str]
) -> None:
    assert ship_env.ship() == 0
    family = _decode(ship_env.out, FAMILY)
    block = next(block for block in family["blocks"].values() if block["registers"])
    block["registers"][0]["offset"] += 4
    (ship_env.out / FAMILY).write_bytes(pack(family))
    capsys.readouterr()
    assert ship_env.ship(NAME, "--check") == 1
    assert "a block hashing to key" in capsys.readouterr().err


@pytest.mark.parametrize(
    ("ranks", "reference"),
    [
        ({NAME: (100, 1024), SIBLING: (90, 2048)}, NAME),
        ({NAME: (90, 1024), SIBLING: (90, 2048)}, SIBLING),
        ({SIBLING: (90, 2048), NAME: (90, 2048)}, NAME),
    ],
    ids=["bonded pins first", "flash second", "name last"],
)
def test_the_reference_of_a_line_is_most_bonded_pins_then_flash_then_name(
    ranks: dict[str, tuple[int, int]], reference: str
) -> None:
    other = {OTHER: (1, 1)}
    assert choose_reference(ranks | other) == {"STM32T001xx": reference, "STM32T101xx": OTHER}


@pytest.mark.parametrize(
    ("file", "family"),
    [
        ("STM32U5xx.blocks.json.xz", "STM32U5xx"),
        ("STM32U575xx.line.json.xz", "STM32U5xx"),
        ("STM32H573xx.parts.json.xz", "STM32H5xx"),
        ("STM32U575QGIx.json.xz", None),
        ("STM32U5xx.blocks.json.xz.tmp.12", None),
    ],
)
def test_layer_family_names_the_family_of_layer_files_only(file: str, family: str | None) -> None:
    assert layer_family(file) == family


def test_write_layers_removes_stale_parts_files_before_their_lines_and_blocks(
    tmp_path: Path,
) -> None:
    stale = ["STM32T0xx.blocks.json.xz", "STM32T001xx.line.json.xz", "STM32T001xx.parts.json.xz"]
    for file in stale:
        (tmp_path / file).write_bytes(b"")
    assert _shipped.write_layers(tmp_path, {}, {"STM32T0xx"}) == stale[::-1]
    assert list(tmp_path.iterdir()) == []
