"""A TinyUSB 0.21.0 composite device, CDC ACM plus HID, enumerated by popcorn's virtual host.

`[board.OTG_FS] host = {}` in `popcorn.toml` plugs a virtual full-speed host into the OTG_FS
model from power-on. It sees the pull-up TinyUSB enables in `tusb_init`, waits the 100 ms
connect debounce, resets the bus, then enumerates as Linux does: device descriptor, a second
reset, SET_ADDRESS 1, descriptors and strings, SET_CONFIGURATION 1. All of it runs natively in
virtual time, in model events: Python only waits for the result.

`machine.periph.OTG_FS.host` is that host. The tests read what it learnt, compare it with the
descriptors compiled into the firmware, and send control requests of their own.
"""

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
