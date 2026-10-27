"""An LL clock setup from the 32 MHz HSI to a 250 MHz PLL1 in VOS0, checked from the host.

SystemCoreClock is the firmware's own computation from the PLL1 registers and the HSI divider.
The real frequency shows in the SysTick period: the core clock follows HCLK, so the virtual time
a period takes depends on the SYSCLK the RCC model computes.
"""

import itertools

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.units import VirtualTime, ms, us

pytestmark = pytest.mark.popcorn(budget=2_000_000)

SYSCLK_HZ = 250_000_000
SWS_PLL1 = 3
FLASH_LATENCY = 5


def timestamps(machine: Machine, function: str) -> list[VirtualTime]:
    seen: list[VirtualTime] = []

    def record(call: Call) -> Return:
        seen.append(machine.time)
        return call.proceed()

    machine.intercept(function, record)
    return seen


async def configure_clock(machine: Machine) -> None:
    stop = await machine.run(until="clock_configured")
    assert stop.reason is StopReason.ADDRESS, stop


async def test_firmware_computes_250_mhz_from_the_pll1_registers(rcc: Machine) -> None:
    await configure_clock(rcc)
    assert rcc.var("SystemCoreClock").value == SYSCLK_HZ
    assert rcc.periph.RCC.RCC_CFGR1.SWS == SWS_PLL1


async def test_voltage_range_and_wait_states_are_set_before_the_clock_is_raised(
    rcc: Machine,
) -> None:
    await configure_clock(rcc)
    assert rcc.periph.PWR.PWR_VOSCR.VOS == 3
    assert rcc.periph.PWR.PWR_VOSSR.VOSRDY == 1
    assert rcc.periph.FLASH.FLASH_ACR.LATENCY == FLASH_LATENCY


async def test_hsi_is_ready_before_pll1_locks(rcc: Machine) -> None:
    ready: dict[str, tuple[int, int]] = {}
    for hook in ("hsi_ready", "pll_ready"):

        def flags(call: Call, hook: str = hook) -> Return:
            cr = rcc.periph.RCC.RCC_CR
            ready[hook] = (cr.HSIRDY, cr.PLL1RDY)
            return call.proceed()

        rcc.intercept(hook, flags)
    await configure_clock(rcc)

    assert ready == {"hsi_ready": (1, 0), "pll_ready": (1, 1)}


async def test_systick_ticks_every_millisecond_at_250_mhz(rcc: Machine) -> None:
    await configure_clock(rcc)
    ticks = timestamps(rcc, "SysTick_Handler")
    stop = await rcc.run(for_=ms(5))
    assert stop.reason is StopReason.TIME, stop

    assert len(ticks) >= 4
    for earlier, later in itertools.pairwise(ticks):
        assert abs((later - earlier).ps - ms(1).ps) < us(1).ps
