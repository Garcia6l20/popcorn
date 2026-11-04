"""A TinyUSB 0.21.0 CDC ACM device on the STM32L476 OTG_FS core, enumerated by popcorn's host
under strict_peripherals.

The firmware runs the core at 48 MHz from MSI range 11 in PLL-mode on the LSE, RM0351 6.2.3,
which also clocks the OTG through CCIPR.CLK48SEL. It sets PWR_CR2.USV through
`HAL_PWREx_EnableVddUSB`, without which the transceiver stays isolated. TinyUSB's `dwc2`
driver runs the core in device mode with the FIFOs in slave mode.

`Periph(host=UsbHostOptions())` on OTG_FS plugs a virtual full-speed host into the L476's OTG
model from power-on. It sees the pull-up TinyUSB enables in `tusb_init`, waits the 100 ms
connect debounce, resets the bus, then enumerates as Linux does. All of it runs natively in
virtual time.
"""

import pytest
from popcorn import Machine, UsbStall
from popcorn.board import UsbState, UsbTransferType
from popcorn.units import s

pytestmark = pytest.mark.popcorn(budget=5_000_000)

GET_DESCRIPTOR = 6
GET_STATUS = 0
STRING = 0x0300
ENGLISH = b"\x04\x03\x09\x04"
CDC, CDC_DATA = 0x02, 0x0A
DEVICE_ADDRESS = 1
MSI_48MHZ = 11
SWS_MSI = 0
CLK48_MSI = 3


async def test_the_host_enumerates_the_device_from_boot(usb_cdc: Machine) -> None:
    host = usb_cdc.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    assert host.state is UsbState.CONFIGURED
    assert host.address == DEVICE_ADDRESS
    assert host.configuration == 1

    device = usb_cdc.var("desc_device")
    assert host.device_descriptor == usb_cdc.mem.read(device.address, device.size)
    assert device.idVendor.value == 0xCAFE
    total = int.from_bytes(host.configuration_descriptor[2:4], "little")
    assert total == len(host.configuration_descriptor) == 9 + 66
    assert host.configuration_descriptor == usb_cdc.mem.read(
        usb_cdc.var("desc_configuration").address, total
    )
    assert host.strings == {
        1: "TinyUSB",
        2: "TinyUSB Device",
        3: "123456",
        4: "TinyUSB CDC",
    }
    assert [(i.number, i.class_code) for i in host.interfaces] == [(0, CDC), (1, CDC_DATA)]
    notification = host.interfaces[0].endpoints[0]
    assert (notification.address, notification.transfer_type) == (0x81, UsbTransferType.INTERRUPT)
    assert usb_cdc.var("usb_mounted").value == 1


async def test_the_core_and_clock_registers_show_the_enumeration(usb_cdc: Machine) -> None:
    host = usb_cdc.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    otg = usb_cdc.periph.OTG_FS
    assert otg.DCFG.DAD == DEVICE_ADDRESS
    assert otg.DCTL.SDIS == 0
    assert otg.GCCFG.PWRDWN == 1
    assert usb_cdc.periph.PWR.CR2.USV == 1
    rcc = usb_cdc.periph.RCC
    assert (rcc.CR.MSIRANGE, rcc.CFGR.SWS) == (MSI_48MHZ, SWS_MSI)
    assert (rcc.BDCR.LSERDY, rcc.CR.MSIPLLEN) == (1, 1)
    assert rcc.CCIPR.CLK48SEL == CLK48_MSI
    assert rcc.AHB2ENR.OTGFSEN == 1


async def test_string_zero_lists_english(usb_cdc: Machine) -> None:
    host = usb_cdc.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    assert await host.control(0x80, GET_DESCRIPTOR, STRING, 0, 255) == ENGLISH


async def test_a_request_to_an_unknown_recipient_stalls(usb_cdc: Machine) -> None:
    host = usb_cdc.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    with pytest.raises(UsbStall):
        await host.control(0x83, GET_STATUS, 0, 0, 2)
    assert await host.control(0x80, GET_STATUS, 0, 0, 2) == b"\x00\x00"
