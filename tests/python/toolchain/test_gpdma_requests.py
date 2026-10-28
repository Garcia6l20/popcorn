import os
import re
from collections.abc import Callable
from pathlib import Path

import pytest

PROJECT = Path(__file__).resolve().parents[3]
TABLE = PROJECT / "native" / "src" / "periph" / "stm32" / "gpdma_requests.hpp"
BUILD = Path(os.environ.get("PCONS_BUILD_DIR", PROJECT / "build"))
LL_DMA = BUILD / "deps" / "src" / "stm32u5-hal-driver" / "Inc" / "stm32u5xx_ll_dma.h"
LL_DMA_H5 = BUILD / "deps" / "src" / "stm32h5-hal-driver" / "Inc" / "stm32h5xx_ll_dma.h"
H5_ONLY_IN_THE_HAL = {
    "ADF1_FLT0",
    "JPEG_RX_REQ",
    "JPEG_TX_REQ",
    "MDF1_FLT0",
    "MDF1_FLT1",
    "MDF1_FLT2",
    "MDF1_FLT3",
    "MDF1_FLT4",
    "MDF1_FLT5",
    "OCTOSPI2",
}


def ll_name(signal: str) -> str:
    """LL_GPDMA1_REQUEST_ suffix of an RM0456 Table 137 signal name."""
    name = signal.removesuffix("_dma").upper()
    name = re.sub(r"_UPD$", "_UP", name)
    name = re.sub(r"_CC(\d)$", r"_CH\1", name)
    name = re.sub(r"_TRG$", "_TRIG", name)
    return "DCMI_PSSI" if name in ("DCMI", "PSSI") else name


def ll_name_h5(signal: str) -> str:
    """LL_GPDMAx_REQUEST_ suffix of an RM0481 Table 142 signal name."""
    name = ll_name(signal)
    name = re.sub(r"^UART(1[01])_", r"USART\1_", name)
    name = name.replace("_TIG", "_TRIG")
    return {"OSPI1": "OCTOSPI1", "DCMI_PSSI": "DCMI"}.get(name, name)


def table(name: str, size: int, translate: Callable[[str], str] = ll_name) -> dict[str, int]:
    found = re.search(
        rf"std::array<gpdma_request, {size}> {name}\{{(.*?)\n\}};", TABLE.read_text(), re.S
    )
    assert found is not None
    rows = re.findall(r'\{"([a-z0-9_]+)",\s*(\d+)\}', found.group(1))
    assert len(rows) == size
    return {translate(signal): int(number) for signal, number in rows}


def hal(controller: str, header: Path = LL_DMA) -> dict[str, int]:
    rows = re.findall(rf"#define LL_{controller}_REQUEST_(\w+)\s+(\d+)U", header.read_text())
    return {name: int(number) for name, number in rows}


@pytest.mark.skipif(not LL_DMA.is_file(), reason="STM32U5 HAL not fetched, no arm-none-eabi-gcc")
def test_gpdma_request_table_matches_the_hal_ll_request_numbers() -> None:
    assert table("gpdma_requests", 127) == hal("GPDMA1")


@pytest.mark.skipif(not LL_DMA.is_file(), reason="STM32U5 HAL not fetched, no arm-none-eabi-gcc")
def test_lpdma_request_table_matches_the_hal_ll_request_numbers() -> None:
    assert table("lpdma_requests", 17) == hal("LPDMA1")


@pytest.mark.skipif(not LL_DMA_H5.is_file(), reason="STM32H5 HAL not fetched, no arm-none-eabi-gcc")
@pytest.mark.parametrize("controller", ["GPDMA1", "GPDMA2"])
def test_h5_gpdma_request_table_matches_the_hal_ll_request_numbers(controller: str) -> None:
    ours = table("h5_gpdma_requests", 138, ll_name_h5)
    theirs = hal(controller, LL_DMA_H5)
    assert {name: number for name, number in theirs.items() if name not in H5_ONLY_IN_THE_HAL} == {
        name: number for name, number in ours.items()
    }
