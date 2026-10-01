"""The USB OTG_FS core brought up by the HAL PCD driver, without USB traffic.

`HAL_PCD_Init` soft-resets the core with GRSTCTL.CSRST, flushes the receive FIFO with RXFFLSH
and every transmit FIFO with TXFFLSH, and polls each bit until the core clears it, waiting for
AHBIDL first. The driver reaches `usb_core_ready` with both HAL calls returning `HAL_OK` once
those bits clear. No host is attached: the core never enumerates and raises no interrupt.
"""

import pytest
from popcorn import Machine, StopReason
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=5_000_000)

HAL_OK = 0


@pytest.mark.xfail(strict=True, reason="otg_fs: GRSTCTL flush bits never clear")
async def test_the_pcd_driver_brings_the_core_up(machine: Machine) -> None:
    stop = await machine.run(until="usb_core_ready", for_=ms(100))
    assert stop.reason is StopReason.ADDRESS, stop
    assert machine.var("init_status").value == HAL_OK
    assert machine.var("start_status").value == HAL_OK
    grstctl = machine.periph.OTG_FS.GRSTCTL
    assert grstctl.AHBIDL == 1
    assert grstctl.CSRST == 0
    assert grstctl.RXFFLSH == 0
    assert grstctl.TXFFLSH == 0
