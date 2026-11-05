"""RCC reset values, SRAM enable bits and the SDMMC2 delay block of the U5 and H5 shipped parts."""

import json
from functools import cache
from typing import Any, cast

import pytest
from popcorn.stm32.description._shipped import compose_shipped

U57X = ["STM32U575QGIx", "STM32U585AIIx"]
H56X_SWAPPED = ["STM32H562IIKx", "STM32H563IIKx", "STM32H573IIKx"]
H562_PARTS = ["STM32H562AGIx", "STM32H562IIKx", "STM32H562ZITx"]
H563_H573 = ["STM32H563IIKx", "STM32H573IIKx"]


@cache
def _description(name: str) -> dict[str, Any]:
    return cast(dict[str, Any], json.loads(compose_shipped(name)))


def _register(name: str, peripheral: str, register: str) -> dict[str, Any]:
    for p in _description(name)["peripherals"]:
        if p["name"] == peripheral:
            for r in p["registers"]:
                if r["name"] == register:
                    return cast(dict[str, Any], r)
    raise AssertionError(f"{name} has no {peripheral}.{register}")


def _lsb(name: str, register: str, field: str) -> int:
    for f in _register(name, "RCC", register)["fields"]:
        if f["name"] == field:
            return cast(int, f["lsb"])
    raise AssertionError(f"{name} RCC.{register} has no {field}")


def _peripherals(name: str) -> set[str]:
    return {p["name"] for p in _description(name)["peripherals"]}


@pytest.mark.parametrize("name", U57X)
def test_the_u575_and_u585_ahb1enr_resets_without_dcache2en(name: str) -> None:
    assert _register(name, "RCC", "AHB1ENR")["reset"] == 0xD0000100


@pytest.mark.parametrize("name", U57X)
def test_the_u575_and_u585_ahb2enr2_resets_with_sram5en_clear(name: str) -> None:
    assert _register(name, "RCC", "AHB2ENR2")["reset"] == 0


@pytest.mark.parametrize("name", U57X)
def test_the_u575_and_u585_ahb2enr1_reset_is_unchanged(name: str) -> None:
    assert _register(name, "RCC", "AHB2ENR1")["reset"] == 0xC0000000


@pytest.mark.parametrize("name", H56X_SWAPPED)
def test_the_h56x_sram2en_and_sram3en_sit_where_rm0481_puts_them(name: str) -> None:
    assert _lsb(name, "AHB2ENR", "SRAM2EN") == 30
    assert _lsb(name, "AHB2ENR", "SRAM3EN") == 31
    assert _register(name, "RCC", "AHB2ENR")["reset"] == 0xC0000000


@pytest.mark.parametrize("name", H562_PARTS)
def test_the_h562_has_no_sdmmc2_delay_block(name: str) -> None:
    names = _peripherals(name)
    assert "SDMMC1" in names
    assert "DLYBSD1" in names
    assert "SDMMC2" not in names
    assert "DLYBSD2" not in names
    assert "SEC_DLYBSD2" not in names


@pytest.mark.parametrize("name", H563_H573)
def test_the_h563_and_h573_keep_sdmmc2_with_its_delay_block(name: str) -> None:
    names = _peripherals(name)
    assert {"SDMMC2", "DLYBSD2", "SEC_DLYBSD2"} <= names
