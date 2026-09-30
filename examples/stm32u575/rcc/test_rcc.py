"""An LL clock setup from a 32 MHz HSE bypass to a 160 MHz PLL1, checked from the host.

SystemCoreClock is the firmware's own computation from the PLL1 registers and HSE_VALUE. The real
frequency shows in the SysTick period: the core clock follows HCLK, so the virtual time a period
takes depends on the HSE rate the board gives the RCC model.
"""

import itertools

import pytest
from popcorn import Call, Machine, Return, StopReason
from popcorn.units import VirtualTime, ms, us

pytestmark = pytest.mark.popcorn(budget=2_000_000)

needs_board_hse = pytest.mark.xfail(
    strict=True,
    reason="no API sets the board HSE rate: the RCC model runs a 16 MHz HSE, the board 32 MHz",
)

SYSCLK_HZ = 160_000_000
SWS_PLL1 = 3


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


async def test_firmware_computes_160_mhz_from_the_pll1_registers(machine: Machine) -> None:
    await configure_clock(machine)
    assert machine.var("SystemCoreClock").value == SYSCLK_HZ
    assert machine.periph.RCC.RCC_CFGR1.SWS == SWS_PLL1


async def test_hse_is_ready_before_pll1_locks(machine: Machine) -> None:
    ready: dict[str, tuple[int, int]] = {}
    for hook in ("hse_ready", "pll_ready"):

        def flags(call: Call, hook: str = hook) -> Return:
            cr = machine.periph.RCC.RCC_CR
            ready[hook] = (cr.HSERDY, cr.PLL1RDY)
            return call.proceed()

        machine.intercept(hook, flags)
    await configure_clock(machine)

    assert ready == {"hse_ready": (1, 0), "pll_ready": (1, 1)}


@needs_board_hse
async def test_systick_ticks_every_millisecond_at_160_mhz(machine: Machine) -> None:
    await configure_clock(machine)
    ticks = timestamps(machine, "SysTick_Handler")
    stop = await machine.run(for_=ms(5))
    assert stop.reason is StopReason.TIME, stop

    assert len(ticks) >= 4
    for earlier, later in itertools.pairwise(ticks):
        assert abs((later - earlier).ps - ms(1).ps) < us(1).ps
