import os
import re
from pathlib import Path

import pytest

PROJECT = Path(__file__).resolve().parents[3]
TABLE = PROJECT / "native" / "src" / "periph" / "stm32" / "gpdma_requests.hpp"
BUILD = Path(os.environ.get("PCONS_BUILD_DIR", PROJECT / "build"))
LL_DMA = BUILD / "deps" / "src" / "stm32u5-hal-driver" / "Inc" / "stm32u5xx_ll_dma.h"


def ll_name(signal: str) -> str:
    """LL_GPDMA1_REQUEST_ suffix of an RM0456 Table 137 signal name."""
    name = signal.removesuffix("_dma").upper()
    name = re.sub(r"_UPD$", "_UP", name)
    name = re.sub(r"_CC(\d)$", r"_CH\1", name)
    name = re.sub(r"_TRG$", "_TRIG", name)
    return "DCMI_PSSI" if name in ("DCMI", "PSSI") else name


def table() -> dict[str, int]:
    rows = re.findall(r'\{"([a-z0-9_]+)",\s*(\d+)\}', TABLE.read_text())
    assert len(rows) == 127
    return {ll_name(signal): int(number) for signal, number in rows}


def hal() -> dict[str, int]:
    rows = re.findall(r"#define LL_GPDMA1_REQUEST_(\w+)\s+(\d+)U", LL_DMA.read_text())
    return {name: int(number) for name, number in rows}


@pytest.mark.skipif(not LL_DMA.is_file(), reason="STM32U5 HAL not fetched, no arm-none-eabi-gcc")
def test_gpdma_request_table_matches_the_hal_ll_request_numbers() -> None:
    assert table() == hal()
