"""A HAL clock setup from an 8 MHz HSE bypass to a 32 MHz PLL in voltage range 1, checked from
the host, on the xe and the xb build.

The HAL sets and clears the RCC_CR enable bits through their bit-band aliases, so the run also
goes through the bit-band region. SystemCoreClock is the firmware's own computation from the PLL
registers and HSE_VALUE. The real frequency shows in the SysTick period: the core clock follows
HCLK, so the virtual time a period takes depends on the HSE rate the board gives the RCC model.
"""

import itertools

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.units import VirtualTime, ms, us

pytestmark = pytest.mark.popcorn(budget=2_000_000)

SYSCLK_HZ = 32_000_000
SWS_PLL = 3
PLLMUL_12 = 4
PLLDIV_3 = 2
VOS_RANGE_1 = 1


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


async def test_firmware_computes_32_mhz_from_the_pll_registers(rcc: Machine) -> None:
    await configure_clock(rcc)
    assert rcc.var("SystemCoreClock").value == SYSCLK_HZ
    cfgr = rcc.periph.RCC.CFGR
    assert (cfgr.SWS, cfgr.PLLSRC, cfgr.PLLMUL, cfgr.PLLDIV) == (SWS_PLL, 1, PLLMUL_12, PLLDIV_3)


async def test_range_1_and_one_wait_state_are_set_before_the_clock_is_raised(
    rcc: Machine,
) -> None:
    await configure_clock(rcc)
    assert rcc.periph.PWR.CR.VOS == VOS_RANGE_1
    assert rcc.periph.PWR.CSR.VOSF == 0
    acr = rcc.periph.FLASH.ACR
    assert (acr.ACC64, acr.PRFTEN, acr.LATENCY) == (1, 0, 1)


async def test_hse_is_bypassed_and_ready_before_the_pll_locks(rcc: Machine) -> None:
    ready: dict[str, tuple[int, int, int]] = {}
    for hook in ("hse_ready", "pll_ready"):

        def flags(call: Call, hook: str = hook) -> Return:
            cr = rcc.periph.RCC.CR
            ready[hook] = (cr.HSEBYP, cr.HSERDY, cr.PLLRDY)
            return call.proceed()

        rcc.intercept(hook, flags)
    await configure_clock(rcc)

    assert ready == {"hse_ready": (1, 1, 0), "pll_ready": (1, 1, 1)}


async def test_systick_ticks_every_millisecond_at_32_mhz(rcc: Machine) -> None:
    await configure_clock(rcc)
    ticks = timestamps(rcc, "SysTick_Handler")
    stop = await rcc.run(for_=ms(5))
    assert stop.reason is StopReason.TIME, stop

    assert len(ticks) >= 4
    for earlier, later in itertools.pairwise(ticks):
        assert abs((later - earlier).ps - ms(1).ps) < us(1).ps
