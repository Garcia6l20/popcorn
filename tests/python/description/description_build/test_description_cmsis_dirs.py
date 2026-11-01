"""The checkout's fetched CMSIS device directories reach the description sources."""

import os
from pathlib import Path

import pytest
from popcorn.stm32.description._sources import find_sources

from popcorn_build import cmsis_dirs
from popcorn_build.cmsis_dirs import (
    VARIABLE,
    cmsis_device_dirs,
    missing_cmsis_packs,
    with_checkout_dirs,
)

from ..conftest import cmsis_usage_error, require_pin_data


def test_the_checkout_dirs_come_first_and_the_set_entries_stay() -> None:
    dirs = [Path("/c/u5/Include"), Path("/c/l1/Include")]
    joined = os.pathsep.join(["/other/h5", "/c/l1/Include", "/other/u5"])
    assert with_checkout_dirs(joined, dirs).split(os.pathsep) == [
        "/c/u5/Include",
        "/c/l1/Include",
        "/other/h5",
        "/other/u5",
    ]
    assert with_checkout_dirs(None, dirs) == os.pathsep.join(["/c/u5/Include", "/c/l1/Include"])


def test_only_fetched_cmsis_device_packages_are_listed(tmp_path: Path) -> None:
    (tmp_path / "deps.toml").write_text(
        '[packages.cmsis-device-u5]\nurl = "u"\n[packages.cmsis-device-l1]\nurl = "l"\n'
        '[packages.stm32l1-hal-driver]\nurl = "h"\n',
        encoding="utf-8",
    )
    build = tmp_path / "build"
    for name in ("cmsis-device-l1", "stm32l1-hal-driver"):
        (build / "deps" / "src" / name / "Include").mkdir(parents=True)
    assert cmsis_device_dirs(tmp_path, build) == [
        build / "deps" / "src" / "cmsis-device-l1" / "Include"
    ]


def test_the_l1_header_comes_from_the_checkout_deps() -> None:
    require_pin_data()
    l1 = [path for path in cmsis_device_dirs() if path.parent.name == "cmsis-device-l1"]
    if not l1:
        pytest.skip("cmsis-device-l1 is not fetched, it needs arm-none-eabi-gcc at configure")
    assert os.environ[VARIABLE].split(os.pathsep)[: len(cmsis_device_dirs())] == [
        str(path) for path in cmsis_device_dirs()
    ]
    assert find_sources("STM32L152RETx").cmsis_header == l1[0] / "stm32l152xe.h"


def test_a_missing_pack_is_a_usage_error_with_the_cross_compiler() -> None:
    error = cmsis_usage_error(["cmsis-device-l1"], "/usr/bin/arm-none-eabi-gcc")
    assert isinstance(error, pytest.UsageError)
    assert "cmsis-device-l1" in str(error)


def test_a_missing_pack_without_the_cross_compiler_leaves_the_skip() -> None:
    assert cmsis_usage_error(["cmsis-device-l1"], None) is None
    assert cmsis_usage_error([], "/usr/bin/arm-none-eabi-gcc") is None


def test_the_cli_names_each_missing_pack_on_stderr(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    (tmp_path / "deps.toml").write_text(
        '[packages.cmsis-device-u5]\nurl = "u"\n[packages.cmsis-device-l1]\nurl = "l"\n',
        encoding="utf-8",
    )
    (tmp_path / "build" / "deps" / "src" / "cmsis-device-u5" / "Include").mkdir(parents=True)
    monkeypatch.setattr(cmsis_dirs, "project_dir", tmp_path)
    monkeypatch.setenv("PCONS_BUILD_DIR", str(tmp_path / "build"))
    monkeypatch.delenv(VARIABLE, raising=False)
    assert missing_cmsis_packs(tmp_path, tmp_path / "build") == ["cmsis-device-l1"]
    cmsis_dirs.main()
    out, err = capsys.readouterr()
    assert out.strip() == str(tmp_path / "build" / "deps" / "src" / "cmsis-device-u5" / "Include")
    assert err.count("lacks") == 1
    assert "cmsis-device-l1" in err
