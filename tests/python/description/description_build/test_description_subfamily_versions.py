"""The patch `[ip] versions` of the PWR SMPS, the U575 and U585 GPIO and the H5 SHA-2 HASH."""

from fnmatch import fnmatchcase
from pathlib import Path

import pytest
from popcorn.stm32 import mcu, shipped_parts
from popcorn.stm32.description import _sources
from popcorn.stm32.description._patches import load_patches

PATCHES = Path(_sources.__file__).parent / "patches"
SMPS = "stm32u5_smps.toml"
GPIO = "stm32u57x.toml"
SHA2 = "stm32h5_hash_sha2.toml"
U5_NAMES = (
    "STM32U535CBTx",
    "STM32U575QGIx",
    "STM32U575QGIxQ",
    "STM32U585CITx",
    "STM32U585CITxQ",
    "STM32U599NJHxQ",
    "STM32U5A9NJHxQ",
)
H5_NAMES = (
    "STM32H503RBTx",
    "STM32H523CETx",
    "STM32H533RETx",
    "STM32H543RETx",
    "STM32H553RETx",
    "STM32H562ZITx",
    "STM32H563ZITxQ",
    "STM32H573AIIx",
)


def applied(name: str, part: str, svd: str) -> bool:
    return bool(load_patches([PATCHES / name], part=part, svd_name=svd))


@pytest.mark.parametrize("part", U5_NAMES)
def test_the_u5_version_patches_select_their_parts(part: str) -> None:
    assert applied(SMPS, part, "STM32U5xx.svd") == part.endswith("Q")
    assert applied(GPIO, part, "STM32U5xx.svd") == part.startswith(("STM32U575", "STM32U585"))


@pytest.mark.parametrize("part", H5_NAMES)
def test_the_h5_sha2_patch_selects_the_sha2_lines(part: str) -> None:
    assert applied(SHA2, part, "STM32H5xx.svd") == part.startswith(
        ("STM32H52", "STM32H53", "STM32H56", "STM32H57")
    )


def test_every_shipped_part_carries_the_version_of_its_sub_family() -> None:
    parts = shipped_parts()
    assert len(parts) == 268
    checked = 0
    for part in parts:
        versions = {p.name: p.ip_version for p in mcu(part).peripherals}
        if part.startswith("STM32U5"):
            ports = [f"GPIO{c}" for c in "ABCDEFGHI"]
            assert {versions[p] for p in ports} == {"STM32U57x_gpio"}, part
            assert versions["LPGPIO1"] == "STM32U5x_gpio_v1_0", part
            smps = "STM32U5_smps_pwr" if part.endswith("Q") else "STM32U5_pwr_v1_0"
            assert versions["PWR"] == smps, part
            checked += 1
        elif part.startswith("STM32H5"):
            assert versions["HASH"] == "STM32H5_hash_sha2", part
            checked += 1
    assert checked == 94


SETS = {
    SMPS: ["PWR"],
    GPIO: [f"GPIO{c}" for c in "ABCDEFGHI"],
    SHA2: ["HASH"],
}


@pytest.mark.parametrize(
    "part",
    ["STM32U575ZITx", "STM32U575ZITxQ", "STM32U585CITxQ", "STM32H562AGIx", "STM32H573IIKx"],
)
def test_each_version_patch_names_the_committed_versions_with_no_st_files(part: str) -> None:
    svd = "STM32U5xx.svd" if part.startswith("STM32U5") else "STM32H5xx.svd"
    committed = {p.name: p.ip_version for p in mcu(part).peripherals if p.alias_of is None}
    for file in (SMPS, GPIO, SHA2):
        for loaded in load_patches([PATCHES / file], part=part, svd_name=svd):
            assert loaded.patch.ip is not None
            assert loaded.patch.ip.versions is not None
            assert len(loaded.patch.ip.versions) == 1
            for glob, version in loaded.patch.ip.versions.items():
                instances = [name for name in committed if fnmatchcase(name, glob)]
                assert instances == SETS[file], file
                assert {committed[name] for name in instances} == {version}, file
