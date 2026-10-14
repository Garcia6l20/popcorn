"""The layering check: includes stay in each library's allowed areas, each source in one list."""

from __future__ import annotations

from pathlib import Path

import pytest

from popcorn_build.check import stages
from popcorn_build.layering import area_of, include_violations, main, misplaced_sources

NATIVE = Path(__file__).resolve().parents[3] / "native"


def write(root: Path, relative: str, text: str = "") -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    native = tmp_path / "native"
    write(native, "include/popcorn/core/machine.hpp", "#include <vector>\n")
    write(native, "include/popcorn/periph/spi.hpp", "#include <popcorn/core/machine.hpp>\n")
    write(native, "include/popcorn/periph/stm32/spi.hpp", "#include <popcorn/periph/spi.hpp>\n")
    write(
        native, "include/popcorn/periph/devices/at25sf.hpp", "#include <popcorn/periph/spi.hpp>\n"
    )
    write(native, "src/periph/register_layer.hpp", "")
    write(native, "src/periph/stm32/models.cpp", '#include "model_base.hpp"\n')
    write(native, "src/periph/stm32/model_base.hpp", "")
    write(native, "src/obs/tb.hpp", "#include <popcorn/arch/arch.hpp>\n")
    write(native, "include/popcorn/arch/arch.hpp", "")
    write(native, "include/popcorn/arch/armv8m/thumb.hpp", "")
    write(native, "src/bind/board.cpp", "#include <popcorn/periph/stm32/spi.hpp>\n")
    return native


def test_the_source_tree_passes() -> None:
    assert include_violations(NATIVE) == []
    assert main([str(NATIVE)]) == 0


def test_the_check_runs_last_in_the_gate() -> None:
    assert stages(asan=False)[-1][0] == "layering"


def test_a_clean_tree_passes(tree: Path) -> None:
    assert include_violations(tree) == []


def test_devices_including_stm32_is_refused(tree: Path) -> None:
    write(
        tree,
        "include/popcorn/periph/devices/at25sf.hpp",
        "#include <popcorn/periph/spi.hpp>\n#include <popcorn/periph/stm32/spi.hpp>\n",
    )
    assert include_violations(tree) == [
        "native/include/popcorn/periph/devices/at25sf.hpp:2: devices includes stm32"
    ]
    assert main([str(tree)]) == 1


def test_obs_including_armv8m_is_refused(tree: Path) -> None:
    write(tree, "src/obs/tb.hpp", "  #  include <popcorn/arch/armv8m/thumb.hpp>\n")
    assert include_violations(tree) == ["native/src/obs/tb.hpp:1: obs includes armv8m"]


def test_a_relative_include_of_another_library_private_header_is_refused(tree: Path) -> None:
    write(tree, "src/periph/stm32/models.cpp", '#include "../register_layer.hpp"\n')
    assert include_violations(tree) == [
        "native/src/periph/stm32/models.cpp:1: stm32 includes a private header of core"
    ]


def test_bind_may_include_anything_and_nothing_may_include_bind(tree: Path) -> None:
    write(tree, "src/bind/gil_locks.hpp", "")
    write(tree, "src/obs/tb.hpp", '#include "../bind/gil_locks.hpp"\n')
    assert include_violations(tree) == ["native/src/obs/tb.hpp:1: obs includes bind"]


def test_an_unmapped_header_is_an_error(tree: Path) -> None:
    write(tree, "include/popcorn/periph/new_bus.hpp", "")
    with pytest.raises(ValueError, match=r"new_bus\.hpp: not mapped to a library"):
        include_violations(tree)


@pytest.mark.parametrize(
    ("relative", "area"),
    [
        ("include/popcorn/export.hpp", "core"),
        ("include/popcorn/periph/usb_host.hpp", "usb"),
        ("include/popcorn/periph/uart.hpp", "periph"),
        ("include/popcorn/arch/arch.hpp", "arch"),
        ("include/popcorn/arch/armv8m/armv8m.hpp", "armv8m"),
        ("src/arch/arch.cpp", "arch"),
        ("src/arch/armv8m/thumb.cpp", "armv8m"),
        ("src/periph/bus.cpp", "periph"),
        ("src/periph/usb/host.cpp", "usb"),
        ("src/obs/lcov.cpp", "obs"),
        ("src/obs/bind.cpp", None),
        ("src/tools/regs_gen.cpp", None),
    ],
)
def test_files_map_to_their_area(relative: str, area: str | None) -> None:
    assert area_of(NATIVE / relative, NATIVE) == area


def test_a_source_in_two_lists_or_in_none_is_reported(tree: Path) -> None:
    lcov = write(tree, "src/obs/lcov.cpp")
    tb = write(tree, "src/obs/tb.cpp")
    models = tree / "src" / "periph" / "stm32" / "models.cpp"
    board = tree / "src" / "bind" / "board.cpp"
    owners = {"obs": [lcov, tb], "stm32": [models], "_core": [board]}
    assert misplaced_sources(tree, owners) == []
    owners["devices"] = [tb]
    owners["obs"] = [tb]
    assert misplaced_sources(tree, owners) == [
        "native/src/obs/lcov.cpp: in no library",
        "native/src/obs/tb.cpp: in obs, devices",
    ]


def test_a_quoted_popcorn_include_resolves_through_the_include_tree(tree: Path) -> None:
    write(
        tree,
        "include/popcorn/periph/devices/at25sf.hpp",
        '#include "popcorn/periph/stm32/spi.hpp"\n',
    )
    assert include_violations(tree) == [
        "native/include/popcorn/periph/devices/at25sf.hpp:1: devices includes stm32"
    ]


def test_an_include_of_a_file_outside_any_area_is_an_error(tree: Path) -> None:
    write(tree, "include/stray.hpp")
    write(tree, "src/obs/tb.hpp", '#include "../../include/stray.hpp"\n')
    with pytest.raises(ValueError, match=r"include/stray\.hpp: not mapped to a library"):
        include_violations(tree)
