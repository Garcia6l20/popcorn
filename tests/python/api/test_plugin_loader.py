"""The private native plugin loader binding, `_core._load_plugin`.

nanobind's stubgen leaves underscored names out of `_core.pyi`, so the test types them itself.
"""

import shutil
from collections.abc import Callable
from pathlib import Path
from typing import Protocol, cast

import popcorn
import pytest
from popcorn import _core

FIXTURES = (
    Path(_core.__file__).resolve().parent.parent / "tests" / "native" / "plugin" / "plugin-fixtures"
)


class LoadedPlugin(Protocol):
    @property
    def name(self) -> str: ...
    @property
    def path(self) -> str: ...
    @property
    def abi_tag(self) -> str: ...
    @property
    def models(self) -> list[str]: ...


load_plugin = cast(Callable[[str], LoadedPlugin], vars(_core)["_load_plugin"])
host_abi_tag = cast(Callable[[], str], vars(_core)["_host_abi_tag"])


def _copy(name: str, into: Path) -> Path:
    source = FIXTURES / f"{name}.so"
    if not source.is_file():
        pytest.skip(f"plugin fixtures not built, {source} missing")
    target = into / f"{name}.so"
    shutil.copyfile(source, target)
    return target


def test_a_declared_plugin_loads_with_the_host_tag(tmp_path: Path) -> None:
    path = _copy("good", tmp_path)
    plugin = load_plugin(str(path))
    assert type(plugin).__name__ == "_Plugin"
    assert plugin.name == "good"
    assert plugin.path == str(path.resolve())
    assert plugin.models == ["fake"]
    assert plugin.abi_tag == host_abi_tag()
    assert load_plugin(str(path)).path == plugin.path


def test_the_host_tag_names_this_popcorn() -> None:
    tag = host_abi_tag()
    assert tag.startswith(f"popcorn {popcorn.version().split()[0]};gcc ")
    assert ";sdk " in tag


def test_a_refused_plugin_raises_config_error(tmp_path: Path) -> None:
    path = _copy("mismatch", tmp_path)
    with pytest.raises(popcorn.ConfigError, match="abi_tag"):
        load_plugin(str(path))


def test_a_declared_plugin_writing_the_registry_raises_config_error(tmp_path: Path) -> None:
    path = _copy("writes_global", tmp_path)
    with pytest.raises(popcorn.ConfigError, match="periph::register_model"):
        load_plugin(str(path))


def test_a_hard_link_to_a_loaded_plugin_returns_its_record(tmp_path: Path) -> None:
    path = _copy("good", tmp_path)
    link = tmp_path / "link.so"
    link.hardlink_to(path)
    assert load_plugin(str(link)).path == load_plugin(str(path)).path


def test_a_hard_link_to_a_refused_plugin_is_refused(tmp_path: Path) -> None:
    path = _copy("ctor_registers", tmp_path)
    with pytest.raises(popcorn.ConfigError, match="static constructors"):
        load_plugin(str(path))
    link = tmp_path / "link.so"
    link.hardlink_to(path)
    with pytest.raises(popcorn.ConfigError, match="static constructors"):
        load_plugin(str(link))
