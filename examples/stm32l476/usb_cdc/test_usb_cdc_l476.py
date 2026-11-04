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

`host.cdc()` is the CDC ACM function as a byte stream. The host's CDC driver sets the line
coding and DTR at configuration, the firmware echoes upper-cased what it receives once DTR is
set. `machine.board.pty("OTG_FS")` bridges the same function to a host pty, which any serial
tool opens.
"""

import asyncio
import itertools
import os
import select

import pytest
from popcorn import Machine, UsbStall
from popcorn.board import UsbState, UsbTransferType
from popcorn.units import ms, s

pytestmark = pytest.mark.popcorn(budget=5_000_000)

GET_DESCRIPTOR = 6
GET_CONFIGURATION = 8
GET_STATUS = 0
STRING = 0x0300
ENGLISH = b"\x04\x03\x09\x04"
CDC, CDC_DATA = 0x02, 0x0A
SET_CONTROL_LINE_STATE = 0x22
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


async def test_a_restored_snapshot_stays_configured_without_enumerating_again(
    usb_cdc: Machine,
) -> None:
    host = usb_cdc.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    configured = usb_cdc.snapshot()
    await host.detach()
    await usb_cdc.run(for_=ms(5))
    assert host.state is UsbState.DETACHED

    usb_cdc.restore(configured)
    assert host.attached
    assert host.state is UsbState.CONFIGURED
    before = usb_cdc.time
    assert await host.control(0x80, GET_CONFIGURATION, 0, 0, 1) == b"\x01"
    assert usb_cdc.time - before < ms(5)


async def test_unplugging_and_plugging_the_cable_enumerates_again(usb_cdc: Machine) -> None:
    host = usb_cdc.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    await host.detach()
    await usb_cdc.run(for_=ms(5))
    assert (host.attached, host.state) == (False, UsbState.DETACHED)
    assert usb_cdc.var("usb_mounted").value == 1

    await host.attach()
    await host.wait_configured(for_=s(1))
    assert host.address == DEVICE_ADDRESS


async def test_the_cdc_function_echoes_upper_cased(usb_cdc: Machine) -> None:
    cdc = usb_cdc.periph.OTG_FS.host.cdc()
    await cdc.write(b"hello")
    assert await cdc.read(5, for_=s(2)) == b"HELLO"
    assert cdc.bound and cdc.dtr and cdc.rts
    assert usb_cdc.var("cdc_connected").value == 1


async def test_wait_ready_waits_for_dtr_after_the_configuration(usb_cdc: Machine) -> None:
    host = usb_cdc.periph.OTG_FS.host
    earlier = len(host.requests)
    cdc = host.cdc()
    await host.wait_configured(for_=s(2))
    assert cdc.bound and not cdc.dtr
    await cdc.wait_ready(for_=ms(20))
    assert cdc.ready and cdc.dtr and cdc.rts
    states = [r for r in host.requests[earlier:] if r.request == SET_CONTROL_LINE_STATE]
    assert (states[-1].status, states[-1].value) == ("completed", 3)


async def test_1000_bytes_come_back_in_order_in_packets_of_64(usb_cdc: Machine) -> None:
    cdc = usb_cdc.periph.OTG_FS.host.cdc()
    await usb_cdc.periph.OTG_FS.host.wait_configured(for_=s(2))
    sent = bytes(b"abcdefghijklmnopqrstuvwxyz"[n % 26] for n in range(1000))
    await cdc.write(sent)
    packets = await cdc.read_packets(16, for_=ms(100))
    assert [len(p.data) for p in packets] == [64] * 15 + [40]
    assert b"".join(p.data for p in packets) == sent.upper()
    assert all(a.at < b.at for a, b in itertools.pairwise(packets))
    assert cdc.queued == 0


async def test_a_restore_drops_unread_bytes_and_sends_unsent_ones_again(
    usb_cdc: Machine,
) -> None:
    cdc = usb_cdc.periph.OTG_FS.host.cdc()
    await usb_cdc.periph.OTG_FS.host.wait_configured(for_=s(1))
    await cdc.write(b"one")
    saved = usb_cdc.snapshot()
    await usb_cdc.run(for_=ms(20))
    assert cdc.available == 3
    assert cdc.queued == 0

    usb_cdc.restore(saved)
    assert cdc.available == 0
    assert cdc.queued == 3
    assert await cdc.read(3, for_=ms(20)) == b"ONE"


async def test_a_snapshot_in_the_middle_of_a_transfer_restores_it(usb_cdc: Machine) -> None:
    cdc = usb_cdc.periph.OTG_FS.host.cdc()
    await usb_cdc.periph.OTG_FS.host.wait_configured(for_=s(1))
    sent = bytes(range(32, 127)) * 10
    await cdc.write(sent)
    await usb_cdc.run(for_=ms(50), stop_when=lambda _: cdc.available >= 128)
    middle = usb_cdc.snapshot()
    first = await cdc.read(len(sent), for_=ms(100))
    assert first == sent.upper()

    usb_cdc.restore(middle)
    assert cdc.queued == len(sent)
    assert cdc.available == 0
    rest = await cdc.read(len(sent), for_=ms(100))
    assert first.endswith(rest)
    assert len(first) - len(rest) >= 128


async def test_unplugging_keeps_the_bytes_received_and_replugging_carries_on(
    usb_cdc: Machine,
) -> None:
    host = usb_cdc.periph.OTG_FS.host
    cdc = host.cdc()
    await cdc.write(b"xyz")
    await usb_cdc.run(for_=s(1), stop_when=lambda _: cdc.available == 3)
    await host.detach()
    assert not cdc.bound
    assert await cdc.read(3) == b"XYZ"
    assert await cdc.read(1, for_=ms(10)) == b""
    await cdc.write(b"more")

    await host.attach()
    await host.wait_configured(for_=s(1))
    assert await cdc.read(4, for_=ms(50)) == b"MORE"
    assert host.cdc() is cdc


def serial_client(path: str, line: bytes) -> bytes:
    """What a serial tool does: open the pty, write, read the answer, close."""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    try:
        os.write(fd, line)
        answer = b""
        while len(answer) < len(line) and select.select([fd], [], [], 1.0)[0]:
            answer += os.read(fd, 64)
        return answer
    finally:
        os.close(fd)


async def test_a_serial_tool_talks_to_the_cdc_function_through_a_pty(usb_cdc: Machine) -> None:
    host = usb_cdc.periph.OTG_FS.host
    earlier = len(host.requests)
    await host.wait_configured(for_=s(1))
    path = usb_cdc.board.pty("OTG_FS")
    assert usb_cdc.board.ptys == {"OTG_FS": path}
    async with usb_cdc.running():
        assert serial_client(path, b"ping") == b"PING"
        (requests,) = await usb_cdc.read(usb_cdc.ref.periph.OTG_FS.host.requests)
        before = len(requests)
        await asyncio.sleep(0.1)
    assert len(host.requests) == before
    assert not [
        r for r in host.requests[earlier:] if r.request == SET_CONTROL_LINE_STATE and r.value != 3
    ]
    with pytest.raises(RuntimeError, match="bridged"):
        await host.cdc().read(1)


SENT = bytes(b"abcdefghijklmnopqrstuvwxyz"[n % 26] for n in range(1000))


async def echoed_packets(machine: Machine) -> list[bytes]:
    """What the device sends back for SENT, packet by packet."""
    host = machine.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    cdc = host.cdc()
    await cdc.write(SENT)
    return [p.data for p in await cdc.read_packets(16, for_=ms(100))]


async def test_the_echo_is_the_same_packets_parked_and_running(usb_cdc: Machine) -> None:
    host = usb_cdc.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    configured = usb_cdc.snapshot()
    parked = await echoed_packets(usb_cdc)

    usb_cdc.restore(configured)
    async with usb_cdc.running():
        running = await echoed_packets(usb_cdc)

    assert running == parked
    assert b"".join(running) == SENT.upper()
    assert [len(packet) for packet in running] == [64] * 15 + [40]


async def test_a_control_request_gives_the_same_answer_parked_and_running(
    usb_cdc: Machine,
) -> None:
    host = usb_cdc.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    configured = usb_cdc.snapshot()
    parked = await host.control(0x80, GET_DESCRIPTOR, STRING | 4, 0x0409, 255)

    usb_cdc.restore(configured)
    async with usb_cdc.running():
        running = await host.control(0x80, GET_DESCRIPTOR, STRING | 4, 0x0409, 255)

    assert running == parked
    assert parked[2:].decode("utf-16-le") == "TinyUSB CDC"
