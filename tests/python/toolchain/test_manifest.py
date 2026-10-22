import tomllib
from pathlib import Path
from typing import Any

from popcorn_build.manifest import is_docs_only, is_firmware_only, packages_to_fetch

MANIFEST: dict[str, dict[str, Any]] = {
    "doctest": {"url": "u", "build": "none"},
    "hal": {"url": "u", "build": "none", "firmware": True},
    "nanobind": {"url": "u", "build": "none", "firmware": False},
    "cmsis": {"url": "u", "build": "none", "firmware": True},
}


def test_firmware_only_packages_are_skipped_without_the_cross_toolchain() -> None:
    assert packages_to_fetch(MANIFEST, with_firmware=False) == ["doctest", "nanobind"]


def test_every_package_is_fetched_with_the_cross_toolchain() -> None:
    assert packages_to_fetch(MANIFEST, with_firmware=True) == list(MANIFEST)


def test_only_a_true_firmware_key_marks_a_package() -> None:
    assert [name for name, entry in MANIFEST.items() if is_firmware_only(entry)] == ["hal", "cmsis"]
    assert not is_firmware_only({"firmware": "yes"})


def test_the_repository_manifest_marks_the_firmware_packages_firmware_only() -> None:
    manifest = tomllib.loads((Path(__file__).parents[3] / "deps.toml").read_text())["packages"]
    firmware = {name for name, entry in manifest.items() if is_firmware_only(entry)}
    assert firmware == {
        "stm32u5-hal-driver",
        "cmsis-device-u5",
        "cmsis-core",
        "freertos-kernel",
        "tinyusb",
    }
    assert set(packages_to_fetch(manifest, with_firmware=False)).isdisjoint(firmware)


def test_docs_only_packages_are_never_fetched_by_a_configure() -> None:
    manifest = {**MANIFEST, "theme": {"url": "u", "build": "none", "docs": True}}
    assert "theme" not in packages_to_fetch(manifest, with_firmware=True)
    assert "theme" not in packages_to_fetch(manifest, with_firmware=False)
    assert is_docs_only(manifest["theme"])
    assert not is_docs_only({"docs": "yes"})
