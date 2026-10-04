"""A TinyUSB 0.21.0 composite device, CDC ACM plus HID, enumerated by popcorn's virtual host.

`[board.OTG_FS] host = {}` in `popcorn.toml` plugs a virtual full-speed host into the OTG_FS
model from power-on. It sees the pull-up TinyUSB enables in `tusb_init`, waits the 100 ms
connect debounce, resets the bus, then enumerates as Linux does: device descriptor, a second
reset, SET_ADDRESS 1, descriptors and strings, SET_CONFIGURATION 1. All of it runs natively in
virtual time, in model events: Python only waits for the result.

`machine.periph.OTG_FS.host` is that host. The tests read what it learnt, compare it with the
descriptors compiled into the firmware, and send control requests of their own.

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
CDC, CDC_DATA, HID = 0x02, 0x0A, 0x03
SET_CONTROL_LINE_STATE = 0x22


async def test_the_host_enumerates_the_device_from_boot(machine: Machine) -> None:
    host = machine.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    assert host.state is UsbState.CONFIGURED
    assert host.address == 1
    assert host.configuration == 1

    device = machine.var("desc_device")
    assert host.device_descriptor == machine.mem.read(device.address, device.size)
    assert device.idVendor.value == 0xCAFE
    total = int.from_bytes(host.configuration_descriptor[2:4], "little")
    assert total == len(host.configuration_descriptor) == 9 + 66 + 25
    assert host.configuration_descriptor == machine.mem.read(
        machine.var("desc_configuration").address, total
    )
    assert host.strings == {
        1: "TinyUSB",
        2: "TinyUSB Device",
        3: "123456",
        4: "TinyUSB CDC",
        5: "TinyUSB HID",
    }
    assert [(i.number, i.class_code) for i in host.interfaces] == [
        (0, CDC),
        (1, CDC_DATA),
        (2, HID),
    ]
    report = host.interfaces[2].endpoints[0]
    assert (report.address, report.transfer_type, report.interval) == (
        0x83,
        UsbTransferType.INTERRUPT,
        10,
    )
    assert machine.var("usb_mounted").value == 1


async def test_string_zero_lists_english(machine: Machine) -> None:
    host = machine.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    assert await host.control(0x80, GET_DESCRIPTOR, STRING, 0, 255) == ENGLISH


async def test_a_request_to_an_unknown_recipient_stalls(machine: Machine) -> None:
    host = machine.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    with pytest.raises(UsbStall):
        await host.control(0x83, GET_STATUS, 0, 0, 2)
    assert await host.control(0x80, GET_STATUS, 0, 0, 2) == b"\x00\x00"


async def test_a_restored_snapshot_stays_configured_without_enumerating_again(
    machine: Machine,
) -> None:
    host = machine.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    await machine.run(for_=ms(20), stop_when=lambda _: host.cdc().dtr)
    configured = machine.snapshot()
    await host.detach()
    await machine.run(for_=ms(5))
    assert host.state is UsbState.DETACHED

    machine.restore(configured)
    assert host.attached
    assert host.state is UsbState.CONFIGURED
    before = machine.time
    assert await host.control(0x80, GET_CONFIGURATION, 0, 0, 1) == b"\x01"
    assert machine.time - before < ms(5)


async def test_unplugging_and_plugging_the_cable_enumerates_again(machine: Machine) -> None:
    host = machine.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    await host.detach()
    await machine.run(for_=ms(5))
    assert (host.attached, host.state) == (False, UsbState.DETACHED)
    assert machine.var("usb_mounted").value == 1

    await host.attach()
    await host.wait_configured(for_=s(1))
    assert host.address == 1


async def test_the_cdc_function_echoes_upper_cased(machine: Machine) -> None:
    cdc = machine.periph.OTG_FS.host.cdc()
    await cdc.write(b"hello")
    assert await cdc.read(5, for_=s(2)) == b"HELLO"
    assert cdc.bound and cdc.dtr and cdc.rts
    assert machine.var("cdc_connected").value == 1


async def test_1000_bytes_come_back_in_order_in_packets_of_64(machine: Machine) -> None:
    cdc = machine.periph.OTG_FS.host.cdc()
    await machine.periph.OTG_FS.host.wait_configured(for_=s(2))
    sent = bytes(b"abcdefghijklmnopqrstuvwxyz"[n % 26] for n in range(1000))
    await cdc.write(sent)
    packets = await cdc.read_packets(16, for_=ms(100))
    assert [len(p.data) for p in packets] == [64] * 15 + [40]
    assert b"".join(p.data for p in packets) == sent.upper()
    assert all(a.at < b.at for a, b in itertools.pairwise(packets))
    assert cdc.queued == 0


async def test_a_restore_drops_unread_bytes_and_sends_unsent_ones_again(
    machine: Machine,
) -> None:
    cdc = machine.periph.OTG_FS.host.cdc()
    await machine.periph.OTG_FS.host.wait_configured(for_=s(1))
    await cdc.write(b"one")
    saved = machine.snapshot()
    await machine.run(for_=ms(20))
    assert cdc.available == 3
    assert cdc.queued == 0

    machine.restore(saved)
    assert cdc.available == 0
    assert cdc.queued == 3
    assert await cdc.read(3, for_=ms(20)) == b"ONE"


async def test_a_snapshot_in_the_middle_of_a_transfer_restores_it(machine: Machine) -> None:
    cdc = machine.periph.OTG_FS.host.cdc()
    await machine.periph.OTG_FS.host.wait_configured(for_=s(1))
    sent = bytes(range(32, 127)) * 10
    await cdc.write(sent)
    await machine.run(for_=ms(50), stop_when=lambda _: cdc.available >= 128)
    middle = machine.snapshot()
    first = await cdc.read(len(sent), for_=ms(100))
    assert first == sent.upper()

    machine.restore(middle)
    assert cdc.queued == len(sent)
    assert cdc.available == 0
    rest = await cdc.read(len(sent), for_=ms(100))
    assert first.endswith(rest)
    assert len(first) - len(rest) >= 128


async def test_unplugging_keeps_the_bytes_received_and_replugging_carries_on(
    machine: Machine,
) -> None:
    host = machine.periph.OTG_FS.host
    cdc = host.cdc()
    await cdc.write(b"xyz")
    await machine.run(for_=s(1), stop_when=lambda _: cdc.available == 3)
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


async def test_a_serial_tool_talks_to_the_cdc_function_through_a_pty(machine: Machine) -> None:
    host = machine.periph.OTG_FS.host
    await host.wait_configured(for_=s(1))
    path = machine.board.pty("OTG_FS")
    assert machine.board.ptys == {"OTG_FS": path}
    async with machine.running():
        assert await asyncio.to_thread(serial_client, path, b"ping") == b"PING"
        before = len(host.requests)
        await asyncio.sleep(0.1)
    assert len(host.requests) == before
    assert not [r for r in host.requests if r.request == SET_CONTROL_LINE_STATE and r.value != 3]
    with pytest.raises(RuntimeError, match="bridged"):
        await host.cdc().read(1)
