"""The private native plugin loader binding, `_core._load_plugin`."""

import shutil
from pathlib import Path

import popcorn
import pytest
from popcorn import _core

FIXTURES = (
    Path(_core.__file__).resolve().parent.parent / "tests" / "native" / "plugin" / "plugin-fixtures"
)


def _copy(name: str, into: Path) -> Path:
    source = FIXTURES / f"{name}.so"
    if not source.is_file():
        pytest.skip(f"plugin fixtures not built, {source} missing")
    target = into / f"{name}.so"
    shutil.copyfile(source, target)
    return target


def test_a_declared_plugin_loads_with_the_host_tag(tmp_path: Path) -> None:
    path = _copy("good", tmp_path)
    plugin = _core._load_plugin(str(path))  # pyright: ignore[reportPrivateUsage]
    assert isinstance(plugin, _core._Plugin)  # pyright: ignore[reportPrivateUsage]
    assert plugin.name == "good"
    assert plugin.path == str(path.resolve())
    assert plugin.models == ["fake"]
    assert plugin.abi_tag == _core._host_abi_tag()  # pyright: ignore[reportPrivateUsage]
    assert _core._load_plugin(str(path)).path == plugin.path  # pyright: ignore[reportPrivateUsage]


def test_the_host_tag_names_this_popcorn() -> None:
    tag = _core._host_abi_tag()  # pyright: ignore[reportPrivateUsage]
    assert tag.startswith(f"popcorn {popcorn.version().split()[0]};gcc ")
    assert ";sdk " in tag


def test_a_refused_plugin_raises_config_error(tmp_path: Path) -> None:
    path = _copy("mismatch", tmp_path)
    with pytest.raises(popcorn.ConfigError, match="abi_tag"):
        _core._load_plugin(str(path))  # pyright: ignore[reportPrivateUsage]


def test_a_declared_plugin_writing_the_registry_raises_config_error(tmp_path: Path) -> None:
    path = _copy("writes_global", tmp_path)
    with pytest.raises(popcorn.ConfigError, match="periph::register_model"):
        _core._load_plugin(str(path))  # pyright: ignore[reportPrivateUsage]


def test_a_hard_link_to_a_loaded_plugin_returns_its_record(tmp_path: Path) -> None:
    path = _copy("good", tmp_path)
    link = tmp_path / "link.so"
    link.hardlink_to(path)
    linked = _core._load_plugin(str(link))  # pyright: ignore[reportPrivateUsage]
    assert linked.path == _core._load_plugin(str(path)).path  # pyright: ignore[reportPrivateUsage]


def test_a_hard_link_to_a_refused_plugin_is_refused(tmp_path: Path) -> None:
    path = _copy("ctor_registers", tmp_path)
    with pytest.raises(popcorn.ConfigError, match="static constructors"):
        _core._load_plugin(str(path))  # pyright: ignore[reportPrivateUsage]
    link = tmp_path / "link.so"
    link.hardlink_to(path)
    with pytest.raises(popcorn.ConfigError, match="static constructors"):
        _core._load_plugin(str(link))  # pyright: ignore[reportPrivateUsage]
