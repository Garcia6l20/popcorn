"""A HAL clock setup from the 4 MHz MSI reset clock to an 80 MHz PLL in voltage range 1, checked
from the host, with SYSCLK / 16 on the MCO pin PA8.

SystemCoreClock is the firmware's own computation from the PLL registers and MSI_VALUE. The real
frequency shows in the SysTick period: the core clock follows HCLK, so the virtual time a period
takes depends on the PLL the RCC model runs.
"""

import itertools

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.units import VirtualTime, ms, us

pytestmark = pytest.mark.popcorn(budget=2_000_000)

SYSCLK_HZ = 80_000_000
SWS_PLL = 3
PLLSRC_MSI = 1
VOS_RANGE_1 = 1
MCOSEL_SYSCLK = 1
MCOPRE_DIV16 = 4
MODE_ALTERNATE = 2


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


async def test_firmware_computes_80_mhz_from_the_pll_registers(rcc: Machine) -> None:
    await configure_clock(rcc)
    assert rcc.var("SystemCoreClock").value == SYSCLK_HZ
    assert rcc.periph.RCC.CFGR.SWS == SWS_PLL
    pll = rcc.periph.RCC.PLLCFGR
    assert (pll.PLLSRC, pll.PLLM, pll.PLLN, pll.PLLR, pll.PLLREN) == (PLLSRC_MSI, 0, 40, 0, 1)


async def test_range_1_is_held_before_the_pll_and_four_wait_states_after_the_setup(
    rcc: Machine,
) -> None:
    """Range 1 is the PWR_CR1 reset range, RM0351 5.4.1: the case pins the configured state,
    not a transition into range 1."""
    seen: dict[str, tuple[int, int, int]] = {}
    for hook in ("range_1_set", "pll_ready"):

        def state(call: Call, hook: str = hook) -> Return:
            seen[hook] = (
                rcc.periph.PWR.CR1.VOS,
                rcc.periph.RCC.CR.PLLRDY,
                rcc.periph.RCC.CFGR.SWS,
            )
            return call.proceed()

        rcc.intercept(hook, state)
    await configure_clock(rcc)

    assert seen == {"range_1_set": (VOS_RANGE_1, 0, 0), "pll_ready": (VOS_RANGE_1, 1, 0)}
    assert rcc.periph.PWR.SR2.VOSF == 0
    assert rcc.periph.FLASH.ACR.LATENCY == 4


async def test_sysclk_over_16_is_routed_to_the_mco_pin_pa8(rcc: Machine) -> None:
    await configure_clock(rcc)
    cfgr = rcc.periph.RCC.CFGR
    assert (cfgr.MCOSEL, cfgr.MCOPRE) == (MCOSEL_SYSCLK, MCOPRE_DIV16)
    assert rcc.periph.GPIOA.MODER.MODER8 == MODE_ALTERNATE
    assert rcc.periph.GPIOA.AFRH.AFRH8 == 0


async def test_systick_ticks_every_millisecond_at_80_mhz(rcc: Machine) -> None:
    await configure_clock(rcc)
    ticks = timestamps(rcc, "SysTick_Handler")
    stop = await rcc.run(for_=ms(5))
    assert stop.reason is StopReason.TIME, stop

    assert len(ticks) >= 4
    for earlier, later in itertools.pairwise(ticks):
        assert abs((later - earlier).ps - ms(1).ps) < us(1).ps
