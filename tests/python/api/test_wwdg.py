"""The STM32L4 and STM32L1 window watchdog: the reset request it raises and the flag it leaves."""

from pathlib import Path

import popcorn
import pytest
from popcorn import Call, Machine, ResetKind, Return, StopReason
from popcorn.units import VirtualTime, ms, s

PREBUILT = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt"
BUILDS = (
    ("STM32L476RCTx", PREBUILT / "l476" / "l4_wwdg_ll.elf", 4_000_000),
    ("STM32L152RETx", PREBUILT / "l152xe" / "l1_wwdg_ll.elf", 2_097_152),
)


def _machine(part: str, elf: Path) -> Machine:
    m = popcorn.MachineDescription(part=part, images=[popcorn.Elf(elf)]).machine()
    m.reset()
    return m


@pytest.mark.parametrize(("part", "elf", "pclk1"), BUILDS)
async def test_the_window_watchdog_requests_a_reset_64_ticks_after_the_last_refresh(
    part: str, elf: Path, pclk1: int
) -> None:
    m = _machine(part, elf)
    seen: list[VirtualTime] = []

    def record(call: Call) -> Return:
        seen.append(m.time)
        return call.proceed()

    m.intercept("refresh", record)
    stop = await m.run(for_=s(2))
    assert stop.reason is StopReason.RESET_REQUEST, stop
    assert ResetKind[stop.detail.upper()] is ResetKind.WINDOW_WATCHDOG
    tick_ps = 8 * 4096 * 1_000_000_000_000 // pclk1
    waited = (stop.time - seen[-1]).ps
    assert 63 * tick_ps < waited <= 64 * tick_ps + ms(1).ps
    assert len(seen) == 50


@pytest.mark.parametrize(("part", "elf", "pclk1"), BUILDS)
async def test_the_reset_leaves_wwdgrstf_for_the_firmware(part: str, elf: Path, pclk1: int) -> None:
    m = _machine(part, elf)
    stop = await m.run(for_=s(2))
    assert stop.reason is StopReason.RESET_REQUEST, stop
    m.reset(ResetKind[stop.detail.upper()])
    assert m.periph.RCC.CSR.WWDGRSTF == 1
    assert m.periph.WWDG.CR.value == 0x7F


L1_PARTS = ("STM32L100RCTx", "STM32L151RETx", "STM32L152RETx", "STM32L162RETx")


@pytest.mark.parametrize("part", L1_PARTS)
def test_every_l1_category_reads_wdgtb_and_wdga_back(part: str) -> None:
    m = popcorn.MachineDescription(part=part, images=[]).machine()
    m.reset()
    m.periph.RCC.APB1ENR.WWDGEN = 1
    m.periph.WWDG.CFR.write(0x1FF)
    assert m.periph.WWDG.CFR.WDGTB == 3
    m.periph.WWDG.CR.write(0xFF)
    assert m.periph.WWDG.CR.WDGA == 1
    m.periph.WWDG.CR.write(0x7F)
    assert m.periph.WWDG.CR.WDGA == 1


@pytest.mark.parametrize("part", ("STM32U575QGIx", "STM32U585AIIx"))
def test_every_u5_line_reads_the_production_option_register(part: str) -> None:
    m = popcorn.MachineDescription(part=part, images=[]).machine()
    m.reset()
    assert m.periph.FLASH.FLASH_OPTR.read() == 0x1FEF_F8AA
