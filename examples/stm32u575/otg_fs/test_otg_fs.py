"""The USB OTG_FS core brought up by the HAL PCD driver, without USB traffic.

`HAL_PCD_Init` soft-resets the core with GRSTCTL.CSRST, flushes the receive FIFO with RXFFLSH
and every transmit FIFO with TXFFLSH, and polls each bit until the core clears it, waiting for
AHBIDL first. popcorn's OTG_FS model clears those bits as soon as they are written and keeps
AHBIDL set, so the driver reaches `usb_core_ready` with both HAL calls returning `HAL_OK`. No
host is attached: the core never enumerates and raises no interrupt. The firmware also reads
the core ID GSNPSID, 0x4F54310A, the value the U5 HAL compares with USB_OTG_CORE_ID_310A.
"""

import pytest
from popcorn import Machine, StopReason
from popcorn.units import ms

pytestmark = pytest.mark.popcorn(budget=5_000_000)

HAL_OK = 0
CORE_ID_310A = 0x4F54310A


async def test_the_pcd_driver_brings_the_core_up(otg_fs: Machine) -> None:
    stop = await otg_fs.run(until="usb_core_ready", for_=ms(100))
    assert stop.reason is StopReason.ADDRESS, stop
    assert otg_fs.var("init_status").value == HAL_OK
    assert otg_fs.var("start_status").value == HAL_OK
    assert otg_fs.var("core_id").value == CORE_ID_310A
    grstctl = otg_fs.periph.OTG_FS.GRSTCTL
    assert grstctl.AHBIDL == 1
    assert grstctl.CSRST == 0
    assert grstctl.RXFFLSH == 0
    assert grstctl.TXFFLSH == 0
    assert not otg_fs.nvic.pending(otg_fs.irq.OTG_FS_IRQn)
