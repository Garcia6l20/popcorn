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

`host.hid(0)` is the button interface: a press on PC13 sends input report 1, and output report
2 sets `hid_led` through SET_REPORT, the interface having no interrupt OUT endpoint. `host.hid(1)`
has an interrupt OUT endpoint and unnumbered reports: the firmware answers each output report
with its bytes inverted, and keeps a 4-byte feature report.
"""

import asyncio
import contextlib
import itertools
import os
import select

import pytest
from popcorn import Machine, UsbStall
from popcorn.board import UsbHost, UsbState, UsbTransferType
from popcorn.units import ms, s

pytestmark = pytest.mark.popcorn(budget=5_000_000)

GET_DESCRIPTOR = 6
GET_CONFIGURATION = 8
GET_STATUS = 0
STRING = 0x0300
ENGLISH = b"\x04\x03\x09\x04"
CDC, CDC_DATA, HID = 0x02, 0x0A, 0x03
SET_CONTROL_LINE_STATE = 0x22
SET_IDLE, SET_REPORT = 0x0A, 0x09
REPORT_DESCRIPTOR = 0x2200


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
    assert total == len(host.configuration_descriptor) == 9 + 66 + 25 + 32
    assert host.configuration_descriptor == machine.mem.read(
        machine.var("desc_configuration").address, total
    )
    assert host.strings == {
        1: "TinyUSB",
        2: "TinyUSB Device",
        3: "123456",
        4: "TinyUSB CDC",
        5: "TinyUSB HID",
        6: "TinyUSB HID IO",
    }
    assert [(i.number, i.class_code) for i in host.interfaces] == [
        (0, CDC),
        (1, CDC_DATA),
        (2, HID),
        (3, HID),
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
    await machine.run(for_=ms(20), stop_when=lambda _: host.hid(1).report_descriptor != b"")
    assert host.hid(1).report_descriptor != b""
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


async def hid_ready(machine: Machine) -> UsbHost:
    """Wait until both HID interfaces have read their report descriptor."""
    host = machine.periph.OTG_FS.host
    await host.wait_configured(for_=s(2))
    await machine.run(for_=ms(50), stop_when=lambda _: host.hid(1).report_descriptor != b"")
    assert host.hid(1).report_descriptor != b""
    return host


async def led_written(machine: Machine, count: int) -> None:
    """Run until the firmware's main loop handled `count` LED output reports: TinyUSB calls
    `tud_hid_set_report_cb` from `tud_task`, after the status stage."""
    await machine.run(for_=ms(5), stop_when=lambda _: machine.var("hid_led_writes").value >= count)


def press(machine: Machine) -> int:
    """Press PC13, return the time of the press in picoseconds."""
    button = machine.periph.GPIOC.pin(13)
    button.level = True
    return machine.time.ps


async def test_the_host_probes_each_hid_interface_as_linux_usbhid(machine: Machine) -> None:
    host = await hid_ready(machine)
    hid = [
        (r.request_type, r.request, r.value, r.index, r.length)
        for r in requests
        if r.index in (2, 3) and r.request in (SET_IDLE, 6)
    ]
    config = host.configuration_descriptor
    sizes = [
        int.from_bytes(config[at + 7 : at + 9], "little")
        for at in range(len(config) - 9)
        if config[at : at + 2] == b"\x09\x21"
    ]
    assert hid == [
        (0x21, SET_IDLE, 0, 2, 0),
    earlier = len(machine.periph.OTG_FS.host.requests)
        (0x81, 6, REPORT_DESCRIPTOR, 2, sizes[0]),
    requests = host.requests[earlier:]
        (0x21, SET_IDLE, 0, 3, 0),
        (0x81, 6, REPORT_DESCRIPTOR, 3, sizes[1]),
    ]
    assert all(r.status == "completed" for r in requests)
    for index, name in enumerate(("desc_hid_report", "desc_hid_io_report")):
        array = machine.var(name)
        assert host.hid(index).report_descriptor == machine.mem.read(array.address, sizes[index])
        assert host.hid(index).bound


async def test_a_button_press_sends_one_report_within_one_interval(machine: Machine) -> None:
    host = await hid_ready(machine)
    hid = host.hid()
    pressed = press(machine)
    [report] = await hid.read_reports(1, for_=ms(50))
    assert report.data == b"\x01\x01"
    assert pressed < report.at.ps <= pressed + ms(11).ps
    assert machine.var("hid_reports").value == 1


async def test_two_presses_5_ms_apart_give_two_reports_5_ms_apart_within_one_interval(
    machine: Machine,
) -> None:
    host = await hid_ready(machine)
    hid = host.hid()
    button = machine.periph.GPIOC.pin(13)
    button.level = True
    await machine.run(for_=ms(2))
    button.level = False
    await machine.run(for_=ms(3))
    button.level = True
    first, second = await hid.read_reports(2, for_=ms(50))
    assert (first.data, second.data) == (b"\x01\x01", b"\x01\x02")
    assert abs((second.at - first.at).ps - ms(5).ps) <= ms(10).ps


async def test_an_output_report_without_an_out_endpoint_goes_as_set_report(
    machine: Machine,
) -> None:
    host = await hid_ready(machine)
    await host.hid().write_report(b"\x02\x5a")
    await led_written(machine, 1)
    assert machine.var("hid_led").value == 0x5A
    last = host.requests[-1]
    assert (last.request_type, last.request, last.value, last.index, last.length) == (
        0x21,
        SET_REPORT,
        0x0202,
        2,
        2,
    )


async def test_an_output_report_goes_on_the_out_endpoint_and_comes_back_inverted(
    machine: Machine,
) -> None:
    host = await hid_ready(machine)
    hid = host.hid(1)
    before = len(host.requests)
    await hid.write_report(b"\x00" + bytes(range(8)))
    [report] = await hid.read_reports(1, for_=ms(20))
    assert report.data == bytes(0xFF - n for n in range(8))
    assert len(host.requests) == before
    assert hid.queued == 0


async def test_feature_reports_read_and_write_as_hidraw(machine: Machine) -> None:
    host = await hid_ready(machine)
    hid = host.hid(1)
    assert await hid.get_feature(0, 5) == b"\x00\xf0\xf1\xf2\xf3"
    await hid.set_feature(b"\x00abcd")
    assert await hid.get_feature(0, 5) == b"\x00abcd"
    assert [(r.value, r.length) for r in host.requests[-3:]] == [(0x0300, 4)] * 3


async def test_get_feature_on_an_interface_without_feature_reports_stalls(
    machine: Machine,
) -> None:
    host = await hid_ready(machine)
    with pytest.raises(UsbStall):
        await host.hid().get_feature(0, 2)
    assert await host.hid().get_feature(1, 2) == b"\x01"


async def test_hid_and_cdc_run_at_once(machine: Machine) -> None:
    host = await hid_ready(machine)
    cdc, hid = host.cdc(), host.hid()
    await cdc.write(b"both")
    press(machine)
    await host.hid(1).write_report(b"\x00\x0f")
    [report] = await hid.read_reports(1, for_=ms(20))
    [echo] = await host.hid(1).read_reports(1, for_=ms(20))
    assert await cdc.read(4, for_=ms(20)) == b"BOTH"
    assert (report.data, echo.data) == (b"\x01\x01", b"\xf0")


async def test_an_output_report_written_unplugged_survives_a_restore(machine: Machine) -> None:
    host = await hid_ready(machine)
    hid = host.hid()
    await host.detach()
    await machine.run(for_=ms(5))
    writing = asyncio.create_task(hid.write_report(b"\x02\x33"))
    await asyncio.sleep(0)
    writing.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await writing
    assert (hid.bound, hid.queued) == (False, 1)
    unplugged = machine.snapshot()

    await host.attach()
    await host.wait_configured(for_=s(1))
    await machine.run(for_=ms(50), stop_when=lambda _: hid.queued == 0)
    await led_written(machine, 1)
    assert machine.var("hid_led").value == 0x33

    machine.restore(unplugged)
    assert (hid.bound, hid.queued, hid.available) == (False, 1, 0)
    assert machine.var("hid_led").value == 0
    await host.attach()
    await host.wait_configured(for_=s(1))
    await machine.run(for_=ms(50), stop_when=lambda _: hid.queued == 0)
    await led_written(machine, 1)
    assert machine.var("hid_led").value == 0x33
    await hid.write_report(b"\x02\x44")
    await led_written(machine, 2)
    assert machine.var("hid_led_writes").value == 2
    assert machine.var("hid_led").value == 0x44


async def test_a_restore_drops_unread_reports(machine: Machine) -> None:
    host = await hid_ready(machine)
    hid = host.hid()
    saved = machine.snapshot()
    press(machine)
    await machine.run(for_=ms(20))
    assert hid.available == 1
    machine.restore(saved)
    assert hid.available == 0
    assert hid.report_descriptor != b""
