"""The virtual USB host of OTG_FS from Python: wiring, cable, enumeration state, control
requests, snapshots, the project, the CDC ACM stream, the HID streams and the USB Audio
2.0 function.

The firmware is the usb_cdc example's: TinyUSB 0.21.0, a CDC ACM plus HID composite device. The
audio tests run the usb_audio example's: a TinyUSB 0.21.0 UAC2 headset, 16 kHz mono 16-bit."""

from __future__ import annotations

import asyncio
import contextlib
import gc
import logging
import os
import re
import sys
import threading
import time
import tty
import weakref
from collections.abc import Callable, Coroutine, Mapping
from pathlib import Path
from typing import Any

import popcorn
import pytest
from popcorn import _core
from popcorn.board import (
    CdcLineCoding,
    CdcPacket,
    HidStream,
    UsbData,
    UsbHost,
    UsbHostOptions,
    UsbState,
)
from popcorn.units import VirtualDuration, VirtualTime, ms, s, us

from .conftest import BOARD_ELF, late_drained_read, run_bounded
from .projects import Silent

HANG = 10.0

pytest_plugins = ["pytester"]

ROOT = Path(__file__).parents[3]
USB_ELF = ROOT / "examples" / "stm32u575" / "usb_cdc" / "prebuilt" / "usb_cdc.elf"
AUDIO_ELF = ROOT / "examples" / "stm32u575" / "usb_audio" / "prebuilt" / "usb_audio.elf"
OTG_ELF = ROOT / "examples" / "stm32u575" / "otg_fs" / "prebuilt" / "otg_fs_init.elf"
BUDGET = 5_000_000

GET_DESCRIPTOR = 6
SET_LINE_CODING = 0x20
GET_LINE_CODING = 0x21
LINE_CODING = bytes([0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08])
SET_CONTROL_LINE_STATE = 0x22


def usb_machine(
    u575: popcorn.MachineDescription,
    options: UsbHostOptions | None = None,
    elf: Path = USB_ELF,
) -> popcorn.Machine:
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(elf),
        usb={"OTG_FS": options or UsbHostOptions()},
    )
    m.reset()
    return m


def configured(u575: popcorn.MachineDescription) -> tuple[popcorn.Machine, UsbHost]:
    m = usb_machine(u575)
    host = m.periph.OTG_FS.host
    asyncio.run(host.wait_configured(for_=s(2)))
    return m, host


def test_python_states_match_the_native_device_states() -> None:
    assert {state.name: state.value for state in UsbState} == {
        state.name.rstrip("_").upper(): state.value for state in _core.UsbDeviceState
    }


def test_the_host_enumerates_from_power_on(u575: popcorn.MachineDescription) -> None:
    m, host = configured(u575)
    assert host.attached
    assert (host.state, host.address, host.configuration) == (UsbState.CONFIGURED, 1, 1)
    assert host.failed_step is None
    assert host.device_descriptor[:2] == b"\x12\x01"
    assert host.strings[2] == "TinyUSB Device"
    assert [i.number for i in host.interfaces] == [0, 1, 2, 3]
    data = host.interfaces[1]
    assert [(e.number, e.is_in, e.max_packet) for e in data.endpoints] == [
        (2, False, 64),
        (2, True, 64),
    ]
    assert m.periph.SEC_OTG_FS.host is host


def test_control_requests_in_and_out(u575: popcorn.MachineDescription) -> None:
    _, host = configured(u575)

    async def body() -> tuple[bytes, bytes, bytes]:
        head = await host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 8)
        sent = await host.control(0x21, SET_LINE_CODING, 0, 0, LINE_CODING)
        read = await host.control(0xA1, GET_LINE_CODING, 0, 0, 7)
        return head, sent, read

    head, sent, read = asyncio.run(body())
    assert head == host.device_descriptor[:8]
    assert sent == b""
    assert read == LINE_CODING


@pytest.mark.parametrize(
    ("args", "match"),
    [
        ((0x100, 6, 0, 0, 1), "request_type"),
        ((0x80, 6, 0x10000, 0, 1), "value"),
        ((0x80, 6, 0, 0, b"\x00"), "IN request"),
        ((0x00, 9, 1, 0, 2), "OUT request"),
        ((0x80, 6, 0, 0, 0x10000), "wLength"),
    ],
)
def test_control_refuses_malformed_requests(
    u575: popcorn.MachineDescription, args: tuple[int, int, int, int, bytes | int], match: str
) -> None:
    host = usb_machine(u575).periph.OTG_FS.host
    with pytest.raises(ValueError, match=match):
        asyncio.run(host.control(*args))


def test_a_stall_raises_usb_stall(u575: popcorn.MachineDescription) -> None:
    _, host = configured(u575)
    with pytest.raises(popcorn.UsbStall) as caught:
        asyncio.run(host.control(0x83, 0, 0, 0, 2))
    assert caught.value.status == "stalled"
    assert isinstance(caught.value, popcorn.UsbError)
    assert isinstance(caught.value, popcorn.EmulationError)


def test_a_device_that_never_answers_raises_usb_timeout(
    u575: popcorn.MachineDescription,
) -> None:
    m = usb_machine(u575, UsbHostOptions(timeout=ms(20)))
    host = m.periph.OTG_FS.host
    asyncio.run(host.wait_configured(for_=s(2)))
    asyncio.run(m.run(for_=ms(20), stop_when=lambda _: host.hid(1).report_descriptor != b""))
    assert host.hid(1).report_descriptor != b""
    m.periph.OTG_FS.GAHBCFG.GINTMSK = 0
    before = m.time
    with pytest.raises(popcorn.UsbTimeout) as caught:
        asyncio.run(host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 18))
    assert caught.value.status == "timed_out"
    assert ms(20) <= m.time - before < ms(25)


def test_a_request_without_a_device_is_cancelled(u575: popcorn.MachineDescription) -> None:
    host = usb_machine(u575, UsbHostOptions(attached=False)).periph.OTG_FS.host
    with pytest.raises(popcorn.UsbError) as caught:
        asyncio.run(host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 18))
    assert caught.value.status == "cancelled"
    assert not isinstance(caught.value, popcorn.UsbStall | popcorn.UsbTimeout)


def test_an_unplugged_cable_is_plugged_from_python(u575: popcorn.MachineDescription) -> None:
    m = usb_machine(u575, UsbHostOptions(attached=False))
    host = m.periph.OTG_FS.host
    assert not host.attached
    with pytest.raises(popcorn.UsbTimeout, match="not configured, state DETACHED") as caught:
        asyncio.run(host.wait_configured(for_=ms(300)))
    assert caught.value.status == "DETACHED"
    assert host.state is UsbState.DETACHED
    asyncio.run(host.attach())
    asyncio.run(host.wait_configured(for_=ms(300)))
    assert host.state is UsbState.CONFIGURED
    m.reset()
    assert not host.attached


def test_a_snapshot_keeps_the_host_and_its_cable(u575: popcorn.MachineDescription) -> None:
    m, host = configured(u575)
    saved = m.snapshot()
    asyncio.run(host.detach())
    assert host.state is UsbState.DETACHED
    m.restore(saved)
    assert host.attached
    assert host.state is UsbState.CONFIGURED
    assert asyncio.run(host.control(0x80, 8, 0, 0, 1)) == b"\x01"

    other = usb_machine(u575, UsbHostOptions(attached=False))
    other.restore(popcorn.Snapshot.from_bytes(saved.to_bytes()))
    assert other.periph.OTG_FS.host.attached
    assert other.periph.OTG_FS.host.state is UsbState.CONFIGURED


def test_a_controller_without_a_host_refuses_the_accessor(
    u575: popcorn.MachineDescription,
) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(OTG_ELF))
    with pytest.raises(popcorn.ModelError, match="no virtual USB host") as caught:
        _ = m.periph.OTG_FS.host
    assert caught.value.peripheral == "OTG_FS"
    with pytest.raises(popcorn.ModelError):
        _ = m.periph.USART1.host


@pytest.mark.parametrize(
    ("usb", "key"),
    [
        ({"USART1": UsbHostOptions()}, "usb.USART1"),
        ({"OTG_FS": UsbHostOptions(), "SEC_OTG_FS": UsbHostOptions()}, "usb.SEC_OTG_FS"),
        ({"OTG_FS": UsbHostOptions(reset=ms(0))}, "usb.OTG_FS"),
    ],
)
def test_usb_options_are_checked(
    u575: popcorn.MachineDescription, usb: dict[str, UsbHostOptions], key: str
) -> None:
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Machine(u575, image=popcorn.Image.elf(USB_ELF), usb=usb)
    assert caught.value.key == key


def project(board: Mapping[str, popcorn.Periph]) -> popcorn.Project:
    return popcorn.Project(
        part="STM32U575QGIx",
        images=[popcorn.Elf(USB_ELF)],
        boot=popcorn.Boot(until="main", budget=BUDGET),
        board=board,
    )


def test_a_project_wires_the_host_and_its_timings(
    u575: popcorn.MachineDescription, tmp_path: Path
) -> None:
    del u575
    host_options = UsbHostOptions(attached=False, debounce=ms(20))
    m = project({"OTG_FS": popcorn.Periph(host=host_options)}).machine()
    m.reset()
    host = m.periph.OTG_FS.host
    assert not host.attached
    asyncio.run(m.run(for_=ms(200)))
    assert host.state is UsbState.DETACHED
    asyncio.run(host.attach())
    start = m.time
    asyncio.run(host.wait_configured(for_=ms(200)))
    assert m.time - start < ms(100)


@pytest.mark.parametrize(
    "board",
    [
        {"USART1": popcorn.Periph(host=UsbHostOptions())},
        {"OTG_FS": popcorn.Periph(model=popcorn.Use(Silent), host=UsbHostOptions())},
    ],
)
def test_a_host_table_needs_a_usb_controller_with_the_built_in_model(
    u575: popcorn.MachineDescription, board: dict[str, popcorn.Periph]
) -> None:
    del u575
    with pytest.raises(popcorn.ConfigError) as caught:
        project(board).machine()
    assert caught.value.key.endswith(".host")


@pytest.fixture(scope="session")
def u575_json(u575: popcorn.MachineDescription) -> Path:
    """The cached STM32U575 description file, found before pytester moves HOME."""
    del u575
    from popcorn.stm32.description import cached_description

    return cached_description("STM32U575QGIx")


def test_a_boot_past_the_enumeration_is_cached_configured(
    u575_json: Path, pytester: pytest.Pytester
) -> None:
    pytester.makeconftest(
        "from pathlib import Path\n\nimport popcorn\nimport pytest\n"
        "from popcorn.board import UsbHostOptions\n\n"
        'pytest_plugins = ["popcorn.pytest"]\n\n\n'
        '@pytest.fixture(scope="session")\n'
        "def popcorn_project():\n"
        "    return popcorn.Project(\n"
        '        part="STM32U575QGIx",\n'
        f"        description=Path({str(u575_json)!r}),\n"
        f"        images=[popcorn.Elf(Path({str(USB_ELF)!r}))],\n"
        f'        boot=popcorn.Boot(until="usb_configured", budget={BUDGET}),\n'
        '        board={"OTG_FS": popcorn.Periph(host=UsbHostOptions())},\n'
        "    )\n"
    )
    pytester.makepyfile(
        "from pathlib import Path\n\n\n"
        "async def test_state(machine):\n"
        "    host = machine.periph.OTG_FS.host\n"
        "    reply = await host.control(0x80, 8, 0, 0, 1)\n"
        "    with Path('states.txt').open('a') as states:\n"
        "        states.write(f'{host.state.name} {host.address} {reply.hex()}\\n')\n"
    )
    summaries: list[str] = []
    for _ in range(2):
        result = pytester.runpytest("-p", "no:asyncio")
        result.assert_outcomes(passed=1)
        summaries.append(next(line for line in result.outlines if "key(s)" in line))
    assert summaries == [
        "popcorn: 1 key(s), 1 boot(s), 0 from cache",
        "popcorn: 1 key(s), 0 boot(s), 1 from cache",
    ]
    states = (pytester.path / "states.txt").read_text().splitlines()
    assert states == ["CONFIGURED 1 01", "CONFIGURED 1 01"]


def line_states(host: UsbHost) -> list[int]:
    return [r.value for r in host.requests if r.request == SET_CONTROL_LINE_STATE]


def test_the_cdc_driver_sets_the_line_coding_then_dtr_and_rts(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    cdc = host.cdc()
    asyncio.run(m.run(for_=ms(10)))
    classes = [
        (r.request_type, r.request, r.value, r.index, r.length)
        for r in host.requests
        if r.request_type == 0x21 and r.index == 0
    ]
    assert classes == [(0x21, SET_LINE_CODING, 0, 0, 7), (0x21, SET_CONTROL_LINE_STATE, 3, 0, 0)]
    assert all(r.status == "completed" for r in host.requests)
    assert (cdc.bound, cdc.dtr, cdc.rts) == (True, True, True)
    assert cdc.line_coding == CdcLineCoding()
    assert cdc.serial_state == 0
    assert host.cdc() is cdc
    assert repr(cdc) == "CdcStream(OTG_FS, 0)"


def test_dtr_off_by_option_leaves_the_firmware_unconnected_until_python_sets_it(
    u575: popcorn.MachineDescription,
) -> None:
    m = usb_machine(u575, UsbHostOptions(dtr=False))
    host = m.periph.OTG_FS.host
    cdc = host.cdc()

    async def body() -> tuple[bytes, bytes]:
        await host.wait_configured(for_=s(2))
        await cdc.write(b"abc")
        silent = await cdc.read(3, for_=ms(30))
        await cdc.set_control_line_state(dtr=True, rts=False)
        return silent, await cdc.read(3, for_=ms(30))

    silent, echoed = asyncio.run(body())
    assert silent == b""
    assert echoed == b"ABC"
    assert line_states(host) == [2, 1]
    assert (cdc.dtr, cdc.rts) == (True, False)
    assert m.var("cdc_connected").value == 1


def test_wait_ready_runs_past_configuration_until_the_control_line_state_ended(
    u575: popcorn.MachineDescription,
) -> None:
    m = usb_machine(u575)
    host = m.periph.OTG_FS.host
    cdc = host.cdc()

    async def body() -> VirtualTime:
        await host.wait_configured(for_=s(2))
        configured_at = m.time
        assert (cdc.bound, cdc.ready, cdc.dtr) == (True, False, False)
        await cdc.wait_ready(for_=ms(20))
        return configured_at

    configured_at = asyncio.run(body())
    assert (cdc.ready, cdc.dtr, cdc.rts) == (True, True, True)
    [state] = [r for r in host.requests if r.request == SET_CONTROL_LINE_STATE]
    assert state.status == "completed"
    assert configured_at < state.at < m.time
    asyncio.run(cdc.wait_ready(for_=ms(1)))


def test_wait_ready_with_dtr_off_waits_for_the_control_line_state_too(
    u575: popcorn.MachineDescription,
) -> None:
    m = usb_machine(u575, UsbHostOptions(dtr=False, rts=False))
    cdc = m.periph.OTG_FS.host.cdc()
    asyncio.run(cdc.wait_ready(for_=s(2)))
    assert (cdc.ready, cdc.dtr, cdc.rts) == (True, False, False)
    assert line_states(m.periph.OTG_FS.host) == [0]
    assert m.var("cdc_connected").value == 0


def test_wait_ready_raises_usb_timeout_unbound_or_not_ready(
    u575: popcorn.MachineDescription,
) -> None:
    m = usb_machine(u575, UsbHostOptions(attached=False))
    host = m.periph.OTG_FS.host
    cdc = host.cdc()
    with pytest.raises(popcorn.UsbTimeout, match="not ready, unbound") as unbound:
        asyncio.run(cdc.wait_ready(for_=ms(5)))
    assert unbound.value.status == "unbound"

    async def configured_then_wait() -> None:
        await host.attach()
        await host.wait_configured(for_=s(1))
        await cdc.wait_ready(for_=us(200))

    with pytest.raises(popcorn.UsbTimeout, match="not ready, not_ready") as pending:
        asyncio.run(configured_then_wait())
    assert pending.value.status == "not_ready"
    assert cdc.bound


def test_ready_follows_the_binding_through_a_restore_and_an_unplug(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    cdc = host.cdc()
    early = m.snapshot()
    asyncio.run(cdc.wait_ready(for_=ms(20)))
    ready = m.snapshot()

    m.restore(early)
    assert (cdc.bound, cdc.ready) == (True, False)
    m.restore(ready)
    assert (cdc.bound, cdc.ready) == (True, True)
    asyncio.run(host.detach())
    assert (cdc.bound, cdc.ready) == (False, False)


def test_dtr_drops_only_on_request(u575: popcorn.MachineDescription) -> None:
    m, host = configured(u575)
    cdc = host.cdc()

    async def body() -> bytes:
        await m.run(for_=ms(10))
        assert m.var("cdc_connected").value == 1
        await cdc.set_control_line_state(dtr=False)
        await m.run(for_=ms(10))
        await cdc.write(b"x")
        return await cdc.read(1, for_=ms(20))

    assert asyncio.run(body()) == b""
    assert m.var("cdc_connected").value == 0
    assert line_states(host) == [3, 2]


def test_set_line_coding_reaches_the_device(u575: popcorn.MachineDescription) -> None:
    m, host = configured(u575)
    cdc = host.cdc()

    async def body() -> bytes:
        await cdc.set_line_coding(9600, data_bits=7, parity="E", stop_bits=2)
        return await host.control(0xA1, GET_LINE_CODING, 0, 0, 7)

    assert asyncio.run(body()) == bytes([0x80, 0x25, 0, 0, 2, 2, 7])
    assert cdc.line_coding == CdcLineCoding(9600, 7, "E", 2)
    del m


@pytest.mark.parametrize(
    ("kwargs", "match"),
    [
        ({"baud": 0}, "baud"),
        ({"data_bits": 9}, "data_bits"),
        ({"parity": "X"}, "parity"),
        ({"stop_bits": 3}, "stop_bits"),
    ],
)
def test_set_line_coding_refuses_bad_fields(
    u575: popcorn.MachineDescription, kwargs: dict[str, Any], match: str
) -> None:
    cdc = usb_machine(u575).periph.OTG_FS.host.cdc()
    with pytest.raises(ValueError, match=match):
        asyncio.run(cdc.set_line_coding(**kwargs))


def test_a_request_with_no_driver_bound_raises(u575: popcorn.MachineDescription) -> None:
    cdc = usb_machine(u575, UsbHostOptions(attached=False)).periph.OTG_FS.host.cdc()
    assert not cdc.bound
    with pytest.raises(popcorn.UsbError, match="no CDC ACM driver") as caught:
        asyncio.run(cdc.set_control_line_state(dtr=False))
    assert caught.value.status == "unbound"


def test_cdc_functions_are_checked_once_the_configuration_is_read(
    u575: popcorn.MachineDescription,
) -> None:
    m = usb_machine(u575)
    host = m.periph.OTG_FS.host
    early = host.cdc(1)
    assert early.function == 1
    with pytest.raises(ValueError, match="0 or more"):
        host.cdc(-1)
    asyncio.run(host.wait_configured(for_=s(2)))
    with pytest.raises(ValueError, match="1 CDC ACM functions, no 2"):
        host.cdc(2)
    with pytest.raises(ValueError, match="no 1"):
        m.board.pty("OTG_FS", function=1)
    with pytest.raises(ValueError, match="UART has no function 1"):
        m.board.pty("USART1", function=1)


def test_a_pty_on_an_alias_is_keyed_by_the_controller(u575: popcorn.MachineDescription) -> None:
    m = usb_machine(u575)
    path = m.board.pty("SEC_OTG_FS")
    assert m.board.ptys == {"OTG_FS": path}
    assert m.board.pty("OTG_FS") == path
    m.board.close()


def test_connect_pty_bridges_the_cdc_function_from_the_project(
    u575: popcorn.MachineDescription,
) -> None:
    del u575
    periph = popcorn.Periph(host=UsbHostOptions(dtr=False, rts=False), connect="pty")
    m = project({"OTG_FS": periph}).machine()
    assert set(m.board.ptys) == {"OTG_FS"}
    m.reset()
    host = m.periph.OTG_FS.host
    asyncio.run(host.wait_configured(for_=s(2)))
    asyncio.run(m.run(for_=ms(10)))
    assert line_states(host) == [0]
    m.board.close()


def test_connect_pty_on_a_controller_without_a_host_is_refused(
    u575: popcorn.MachineDescription,
) -> None:
    del u575
    with pytest.raises(popcorn.ModelError):
        project({"OTG_FS": popcorn.Periph(connect="pty")}).machine()


def test_bytes_written_unplugged_survive_a_restore_into_a_fresh_machine(
    u575: popcorn.MachineDescription,
) -> None:
    m = usb_machine(u575, UsbHostOptions(attached=False))
    asyncio.run(m.periph.OTG_FS.host.cdc().write(b"queued"))
    saved = popcorn.Snapshot.from_bytes(m.snapshot().to_bytes())

    other = usb_machine(u575, UsbHostOptions(attached=False))
    host = other.periph.OTG_FS.host
    cdc = host.cdc()
    asyncio.run(cdc.write(b"stale"))
    other.restore(saved)
    assert not host.attached
    assert cdc.queued == 6

    async def body() -> bytes:
        await host.attach()
        await host.wait_configured(for_=s(1))
        return await cdc.read(6, for_=ms(50))

    assert asyncio.run(body()) == b"QUEUED"


async def queue_report(hid: HidStream, data: bytes) -> None:
    """Queue an output report without waiting for the device: the write is cancelled once its
    run started, which leaves the report queued."""
    writing = asyncio.create_task(hid.write_report(data))
    await asyncio.sleep(0)
    writing.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await writing


def test_hid_streams_exist_before_configuration_and_are_checked_after(
    u575: popcorn.MachineDescription,
) -> None:
    m = usb_machine(u575)
    host = m.periph.OTG_FS.host
    early = host.hid(1)
    assert (early.function, early.bound, early.report_descriptor) == (1, False, b"")
    with pytest.raises(ValueError, match="0 or more"):
        host.hid(-1)
    asyncio.run(host.wait_configured(for_=s(2)))
    asyncio.run(m.run(for_=ms(20), stop_when=lambda _: early.report_descriptor != b""))
    assert early.report_descriptor != b""
    assert host.hid(1) is early
    assert early.bound
    assert repr(early) == "HidStream(OTG_FS, 1)"
    with pytest.raises(ValueError, match="2 HID functions, no 2"):
        host.hid(2)


def test_a_feature_request_with_no_hid_driver_bound_raises(
    u575: popcorn.MachineDescription,
) -> None:
    hid = usb_machine(u575, UsbHostOptions(attached=False)).periph.OTG_FS.host.hid()
    with pytest.raises(popcorn.UsbError, match="no HID driver") as caught:
        asyncio.run(hid.get_feature(1, 2))
    assert caught.value.status == "unbound"
    with pytest.raises(popcorn.UsbError, match="no HID driver"):
        asyncio.run(hid.set_feature(b"\x01\x02"))


BAD_HID_CALLS: list[tuple[Callable[[HidStream], Coroutine[Any, Any, object]], str]] = [
    (lambda hid: hid.write_report(b""), "at least 2 bytes"),
    (lambda hid: hid.write_report(b"\x02"), "at least 2 bytes"),
    (lambda hid: hid.write_report(b"\x02" + bytes(0xFFFF)), "at most 0xffff"),
    (lambda hid: hid.write_report(b"\x00" + bytes(0x10000)), "at most 0xffff"),
    (lambda hid: hid.get_feature(256, 2), "report ID"),
    (lambda hid: hid.get_feature(1, 0), "length"),
    (lambda hid: hid.set_feature(b""), "at least"),
    (lambda hid: hid.read_reports(-1), "cannot read"),
]


@pytest.mark.parametrize(("call", "match"), BAD_HID_CALLS)
def test_hid_calls_refuse_bad_arguments(
    u575: popcorn.MachineDescription,
    call: Callable[[HidStream], Coroutine[Any, Any, object]],
    match: str,
) -> None:
    hid = usb_machine(u575, UsbHostOptions(attached=False)).periph.OTG_FS.host.hid()
    with pytest.raises(ValueError, match=match):
        asyncio.run(call(hid))


def test_an_output_report_written_unplugged_survives_a_restore_into_a_fresh_machine(
    u575: popcorn.MachineDescription,
) -> None:
    m = usb_machine(u575, UsbHostOptions(attached=False))
    asyncio.run(queue_report(m.periph.OTG_FS.host.hid(), b"\x02\x77"))
    assert m.periph.OTG_FS.host.hid().queued == 1
    saved = popcorn.Snapshot.from_bytes(m.snapshot().to_bytes())

    other = usb_machine(u575, UsbHostOptions(attached=False))
    host = other.periph.OTG_FS.host
    hid = host.hid()
    asyncio.run(queue_report(hid, b"\x02\x11"))
    asyncio.run(queue_report(hid, b"\x02\x12"))
    assert hid.queued == 2
    other.restore(saved)
    assert not host.attached
    assert hid.queued == 1

    async def body() -> None:
        await host.attach()
        await host.wait_configured(for_=s(1))
        await other.run(for_=ms(50), stop_when=lambda _: other.var("hid_led_writes").value >= 1)

    asyncio.run(body())
    assert (hid.queued, other.var("hid_led").value, other.var("hid_led_writes").value) == (
        0,
        0x77,
        1,
    )


def test_a_report_whose_await_is_cancelled_before_it_is_queued_is_not_done(
    u575: popcorn.MachineDescription,
) -> None:
    m = usb_machine(u575, UsbHostOptions(attached=False))
    hid = m.periph.OTG_FS.host.hid()
    at = m.time + ms(1)
    writing = hid.write_report(b"\x02\x55", at=at)

    async def cancelled() -> None:
        task = asyncio.create_task(writing)
        await asyncio.sleep(0)
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(cancelled())
    assert (writing.done(), hid.queued) == (False, 0)
    asyncio.run(m.run(for_=ms(2)))
    assert (writing.done(), hid.queued) == (True, 1)


def test_a_report_command_left_by_a_cancel_reports_its_later_failure(
    u575: popcorn.MachineDescription, monkeypatch: pytest.MonkeyPatch
) -> None:
    m = usb_machine(u575, UsbHostOptions(attached=False))
    hid = m.periph.OTG_FS.host.hid()
    reported: list[tuple[str | None, type[object], str]] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append((raised.err_msg, type(raised.exc_value), str(raised.exc_value)))

    monkeypatch.setattr(sys, "unraisablehook", report)
    writing = hid.write_report(b"\x02\x55", at=m.time + ms(1))
    gone = weakref.ref(writing)

    async def cancelled(pending: Coroutine[Any, Any, VirtualTime]) -> None:
        task = asyncio.create_task(pending)
        await asyncio.sleep(0)
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(cancelled(writing))
    del writing
    gc.collect()
    assert gone() is None
    assert reported == []
    m.reset()
    assert len(reported) == 1
    assert reported[0][0] == "a command its awaiter cancelled failed later"
    assert "superseded by a reset" in reported[0][2]


def test_an_unplugged_function_keeps_its_report_descriptor_across_a_restore(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    hid = host.hid()
    asyncio.run(m.run(for_=ms(20), stop_when=lambda _: hid.report_descriptor != b""))
    descriptor = hid.report_descriptor
    assert descriptor != b""
    asyncio.run(host.detach())
    assert (hid.bound, hid.report_descriptor) == (False, descriptor)
    saved = popcorn.Snapshot.from_bytes(m.snapshot().to_bytes())
    m.restore(saved)
    assert (hid.bound, hid.report_descriptor) == (False, descriptor)

    fresh = usb_machine(u575)
    fresh.restore(saved)
    assert fresh.periph.OTG_FS.host.hid().report_descriptor == descriptor


def test_hid_wait_ready_runs_until_the_report_descriptor_is_read(
    u575: popcorn.MachineDescription,
) -> None:
    _, host = configured(u575)
    first, second = host.hid(0), host.hid(1)
    assert (second.bound, second.ready, second.report_descriptor) == (True, False, b"")
    asyncio.run(second.wait_ready(for_=ms(50)))
    assert first.ready and second.ready
    assert second.report_descriptor != b""
    asyncio.run(host.detach())
    assert (second.bound, second.ready) == (False, False)
    with pytest.raises(popcorn.UsbTimeout, match="not ready, unbound"):
        asyncio.run(second.wait_ready(for_=ms(5)))


def audio_machine(
    u575: popcorn.MachineDescription, options: UsbHostOptions | None = None
) -> popcorn.Machine:
    return usb_machine(u575, options, AUDIO_ELF)


def test_audio_functions_exist_before_configuration_and_are_checked_after(
    u575: popcorn.MachineDescription,
) -> None:
    m = audio_machine(u575)
    host = m.periph.OTG_FS.host
    early = host.audio(1)
    assert (early.function, early.bound, early.ready, early.feature_units) == (1, False, False, ())
    assert (early.speaker.bound, early.speaker.format, early.speaker.rate) == (False, None, 0)
    with pytest.raises(ValueError, match="0 or more"):
        host.audio(-1)
    audio = host.audio()
    asyncio.run(audio.wait_ready(for_=s(2)))
    assert host.audio() is audio
    assert repr(audio) == "UacFunction(OTG_FS, 0)"
    assert (repr(audio.speaker), repr(audio.mic)) == ("UacSpeaker(OTG_FS, 0)", "UacMic(OTG_FS, 0)")
    with pytest.raises(ValueError, match="1 audio functions, no 2"):
        host.audio(2)


def test_audio_wait_ready_raises_unbound_when_unplugged(u575: popcorn.MachineDescription) -> None:
    audio = audio_machine(u575, UsbHostOptions(attached=False)).periph.OTG_FS.host.audio()
    with pytest.raises(popcorn.UsbTimeout) as caught:
        asyncio.run(audio.wait_ready(for_=ms(5)))
    assert caught.value.status == "unbound"
    with pytest.raises(popcorn.UsbError) as refused:
        asyncio.run(audio.mic.start())
    assert refused.value.status == "unbound"
    with pytest.raises(popcorn.UsbError, match="no audio driver"):
        asyncio.run(audio.set_mute(2, 0, True))
    asyncio.run(audio.speaker.write([1, 2]))
    asyncio.run(audio.speaker.stop())
    assert audio.speaker.queued == 0


def test_a_drain_right_after_start_waits_for_the_first_packet_on_the_bus(
    u575: popcorn.MachineDescription,
) -> None:
    m = audio_machine(u575)
    speaker = m.periph.OTG_FS.host.audio().speaker

    async def body() -> VirtualTime:
        await m.periph.OTG_FS.host.audio().wait_ready(for_=s(2))
        await speaker.start()
        written = m.time
        await speaker.write([1, 2, 3])
        await speaker.drain(for_=ms(10))
        return written

    written = asyncio.run(body())
    assert speaker.queued == 0
    assert not speaker.sending
    sent = speaker.sent_at
    assert sent is not None
    assert sent >= written


@pytest.mark.parametrize("drop", ["stop", "unplug"])
def test_a_drain_raises_stopped_when_the_last_packet_is_dropped_before_the_bus(
    u575: popcorn.MachineDescription, drop: str
) -> None:
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(AUDIO_ELF),
        usb={"OTG_FS": UsbHostOptions()},
        slice_instructions=1000,
    )
    m.reset()
    host = m.periph.OTG_FS.host
    speaker = host.audio().speaker

    async def body() -> str:
        await host.audio().wait_ready(for_=s(2))
        await speaker.start()
        await speaker.write([1, 2, 3])
        await m.run(for_=ms(5), stop_when=lambda _: speaker.queued == 0 and speaker.sending)
        assert speaker.queued == 0 and speaker.sending
        at = m.time + VirtualDuration(1)
        stamped = speaker.stop(at=at) if drop == "stop" else host.detach(at=at)
        with pytest.raises(popcorn.UsbError) as caught:
            await speaker.drain()
        await stamped
        return caught.value.status

    assert asyncio.run(body()) == "stopped"
    assert not speaker.sending
    assert speaker.sent_at is None


def test_a_stop_during_drain_raises_stopped(u575: popcorn.MachineDescription) -> None:
    m = audio_machine(u575)
    speaker = m.periph.OTG_FS.host.audio().speaker

    async def body() -> str:
        await m.periph.OTG_FS.host.audio().wait_ready(for_=s(2))
        await speaker.start()
        await speaker.write([1] * 1600)
        stopped = speaker.stop(at=m.time + ms(10))
        async with m.running():
            with pytest.raises(popcorn.UsbError) as caught:
                await speaker.drain()
        await stopped
        return caught.value.status

    assert asyncio.run(body()) == "stopped"
    assert speaker.queued == 0


def test_speaker_writes_refuse_bad_samples(u575: popcorn.MachineDescription) -> None:
    m = audio_machine(u575)
    audio = m.periph.OTG_FS.host.audio()
    speaker = audio.speaker
    with pytest.raises(ValueError, match="4-byte sample"):
        asyncio.run(speaker.write([1 << 31]))
    asyncio.run(audio.wait_ready(for_=s(2)))
    with pytest.raises(ValueError, match="2-byte sample"):
        asyncio.run(speaker.write([0x8000]))
    with pytest.raises(ValueError, match="2-byte sample"):
        asyncio.run(speaker.write([True]))
    asyncio.run(speaker.write([-0x8000, 0x7FFF]))
    assert speaker.queued == 2
    with pytest.raises(ValueError, match="-32768 to 32767"):
        asyncio.run(audio.set_volume(2, 0, 0x8000))
    with pytest.raises(ValueError, match="cannot read -1 frames"):
        asyncio.run(audio.mic.read_frames(-1))


def test_drain_raises_while_the_speaker_does_not_run(u575: popcorn.MachineDescription) -> None:
    m = audio_machine(u575)
    audio = m.periph.OTG_FS.host.audio()
    asyncio.run(audio.wait_ready(for_=s(2)))
    asyncio.run(audio.speaker.write([1, 2, 3]))
    with pytest.raises(popcorn.UsbError) as caught:
        asyncio.run(audio.speaker.drain())
    assert caught.value.status == "stopped"
    assert audio.speaker.queued == 3
    assert audio.speaker.sent_at is None


def test_a_start_inside_running_streams_from_the_background_run(
    u575: popcorn.MachineDescription,
) -> None:
    m = audio_machine(u575)
    audio = m.periph.OTG_FS.host.audio()

    async def body() -> int:
        await audio.wait_ready(for_=s(2))
        async with m.running():
            await audio.mic.start()
            frames = await audio.mic.read_frames(5, for_=ms(20))
            await audio.mic.stop()
        return len(frames)

    assert asyncio.run(body()) == 5
    assert not audio.mic.running


def test_speaker_samples_written_unplugged_survive_a_restore_into_a_fresh_machine(
    u575: popcorn.MachineDescription,
) -> None:
    m = audio_machine(u575, UsbHostOptions(attached=False))
    asyncio.run(m.periph.OTG_FS.host.audio().speaker.write([5, 6, 7]))
    saved = popcorn.Snapshot.from_bytes(m.snapshot().to_bytes())

    other = audio_machine(u575, UsbHostOptions(attached=False))
    host = other.periph.OTG_FS.host
    speaker = host.audio().speaker
    asyncio.run(speaker.write([1] * 10))
    other.restore(saved)
    assert speaker.queued == 3

    async def body() -> None:
        await host.attach()
        audio = host.audio()
        await audio.wait_ready(for_=s(1))
        await audio.speaker.start()
        await audio.speaker.drain(for_=ms(10))

    asyncio.run(body())
    assert speaker.queued == 0
    assert speaker.sent_at is not None


def test_a_restore_drops_mic_frames_and_keeps_the_streams_running(
    u575: popcorn.MachineDescription,
) -> None:
    m = audio_machine(u575)
    audio = m.periph.OTG_FS.host.audio()

    async def start() -> None:
        await audio.wait_ready(for_=s(2))
        await audio.mic.start()
        await m.run(for_=ms(5))

    asyncio.run(start())
    saved = m.snapshot()
    asyncio.run(m.run(for_=ms(5)))
    assert audio.mic.available >= 5
    asyncio.run(audio.mic.stop())
    assert not audio.mic.running
    m.restore(saved)
    assert audio.mic.available == 0
    assert audio.mic.dropped == 0
    assert audio.mic.running
    frames = asyncio.run(audio.mic.read_frames(3, for_=ms(5)))
    assert len(frames) == 3


def gil_entries(m: popcorn.Machine) -> int:
    return m._handle.gil_entries()  # pyright: ignore[reportPrivateUsage]


def posted_without_the_gil(m: popcorn.Machine, post: Callable[[], Any]) -> None:
    """Post a command inside a free run and await it: it completes, and the machine's thread
    never took the GIL."""
    before = gil_entries(m)

    async def scenario() -> None:
        async with m.running():
            posted = post()
            if posted is not None:
                await posted

    asyncio.run(scenario())
    assert gil_entries(m) == before


def usb_link_of(owner: object) -> Any:
    linked: Any = owner
    return linked._link


type Command = Callable[[popcorn.Machine, UsbHost, Any, Any], Any]

CDC_COMMANDS: dict[str, Command] = {
    "cdc-write": lambda m, host, cdc, hid: cdc.write(b"hi"),
    "cdc-send": lambda m, host, cdc, hid: m._post(  # pyright: ignore[reportPrivateUsage]
        usb_link_of(cdc).command_send(b"hi")
    ),
    "cdc-line-coding": lambda m, host, cdc, hid: m._post(  # pyright: ignore[reportPrivateUsage]
        usb_link_of(cdc).command_set_line_coding(9600, 0, 0, 8)
    ),
    "cdc-control-lines": lambda m, host, cdc, hid: m._post(  # pyright: ignore[reportPrivateUsage]
        usb_link_of(cdc).command_set_control_line_state(True, True)
    ),
    "hid-write": lambda m, host, cdc, hid: m._post(  # pyright: ignore[reportPrivateUsage]
        usb_link_of(hid).command_write(b"\x00\x01")
    ),
    "hid-get-report": lambda m, host, cdc, hid: m._post(  # pyright: ignore[reportPrivateUsage]
        usb_link_of(hid).command_get_report(3, 0, 8)
    ),
    "hid-set-report": lambda m, host, cdc, hid: m._post(  # pyright: ignore[reportPrivateUsage]
        usb_link_of(hid).command_set_report(3, 0, b"\x01")
    ),
    "control": lambda m, host, cdc, hid: m._post(  # pyright: ignore[reportPrivateUsage]
        usb_link_of(host).command_control(usb_link_of(host).ticket(), 0x80, 6, 0x0100, 0, 18, b"")
    ),
    "detach": lambda m, host, cdc, hid: host.detach(),
    "attach": lambda m, host, cdc, hid: host.attach(),
    "time": lambda m, host, cdc, hid: m._post(None),  # pyright: ignore[reportPrivateUsage]
}


@pytest.mark.parametrize("kind", list(CDC_COMMANDS))
def test_a_usb_command_runs_on_the_machine_thread_without_the_gil(
    u575: popcorn.MachineDescription, kind: str
) -> None:
    m, host = configured(u575)
    cdc, hid = host.cdc(), host.hid(0)
    asyncio.run(host.hid(1).wait_ready(for_=ms(50)))
    posted_without_the_gil(m, lambda: CDC_COMMANDS[kind](m, host, cdc, hid))


UAC_COMMANDS: dict[str, Callable[[popcorn.Machine, Any], Any]] = {
    "uac-start": lambda m, audio: m._post(  # pyright: ignore[reportPrivateUsage]
        usb_link_of(audio).command_start(False)
    ),
    "uac-stop": lambda m, audio: audio.speaker.stop(),
    "uac-write": lambda m, audio: audio.speaker.write([0, 0]),
    "uac-set-feature": lambda m, audio: m._post(  # pyright: ignore[reportPrivateUsage]
        usb_link_of(audio).command_set_feature(2, 0, 1, 1)
    ),
}


@pytest.mark.parametrize("kind", list(UAC_COMMANDS))
def test_an_audio_command_runs_on_the_machine_thread_without_the_gil(
    u575: popcorn.MachineDescription, kind: str
) -> None:
    m = audio_machine(u575)
    audio = m.periph.OTG_FS.host.audio()
    asyncio.run(audio.wait_ready(for_=s(2)))
    posted_without_the_gil(m, lambda: UAC_COMMANDS[kind](m, audio))


type UsbCall = Callable[[UsbHost, Any, Any], Coroutine[Any, Any, object]]


async def cdc_read(host: UsbHost, cdc: Any, hid: Any) -> object:
    await cdc.write(b"abc")
    return await cdc.read(3, for_=ms(50))


async def cdc_read_packets(host: UsbHost, cdc: Any, hid: Any) -> object:
    await cdc.write(b"abc")
    return await cdc.read_packets(1, for_=ms(50))


USB_CALLS: dict[str, UsbCall] = {
    "cdc-read": cdc_read,
    "cdc-read-packets": cdc_read_packets,
    "cdc-ready": lambda host, cdc, hid: cdc.wait_ready(for_=ms(50)),
    "cdc-line-coding": lambda host, cdc, hid: cdc.set_line_coding(9600),
    "cdc-control-lines": lambda host, cdc, hid: cdc.set_control_line_state(dtr=True),
    "hid-ready": lambda host, cdc, hid: hid.wait_ready(for_=ms(50)),
    "hid-write-report": lambda host, cdc, hid: hid.write_report(b"\x02\x11"),
    "hid-get-feature": lambda host, cdc, hid: hid.get_feature(1, 2),
    "hid-set-feature": lambda host, cdc, hid: hid.set_feature(b"\x01\x02"),
    "hid-read-reports": lambda host, cdc, hid: hid.read_reports(1, for_=ms(20)),
    "control": lambda host, cdc, hid: host.control(0x80, 6, 0x0100, 0, 18),
    "configured": lambda host, cdc, hid: host.wait_configured(for_=ms(10)),
    "attach": lambda host, cdc, hid: host.attach(),
}


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
@pytest.mark.parametrize("kind", list(USB_CALLS))
def test_a_usb_call_waits_on_the_machine_thread_without_the_gil(
    u575: popcorn.MachineDescription, kind: str, running: bool
) -> None:
    m, host = configured(u575)
    cdc, hid = host.cdc(), host.hid(0 if kind != "hid-ready" else 1)

    async def scenario() -> None:
        if running:
            async with m.running():
                await USB_CALLS[kind](host, cdc, hid)
        else:
            await USB_CALLS[kind](host, cdc, hid)

    asyncio.run(scenario())
    assert gil_entries(m) == 0


async def speaker_drained(audio: Any) -> object:
    await audio.speaker.start()
    await audio.speaker.write([1, 2, 3, 4])
    return await audio.speaker.drain(for_=ms(10))


async def mic_read(audio: Any) -> object:
    await audio.mic.start()
    return await audio.mic.read_frames(2, for_=ms(20))


UAC_CALLS: dict[str, Callable[[Any], Coroutine[Any, Any, object]]] = {
    "ready": lambda audio: audio.wait_ready(for_=ms(10)),
    "speaker-start-write-drain": speaker_drained,
    "mic-start-read": mic_read,
    "set-volume": lambda audio: audio.set_volume(2, 0, 0x100),
}


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
@pytest.mark.parametrize("kind", list(UAC_CALLS))
def test_an_audio_call_waits_on_the_machine_thread_without_the_gil(
    u575: popcorn.MachineDescription, kind: str, running: bool
) -> None:
    m = audio_machine(u575)
    audio = m.periph.OTG_FS.host.audio()
    asyncio.run(audio.wait_ready(for_=s(2)))

    async def scenario() -> None:
        if running:
            async with m.running():
                await UAC_CALLS[kind](audio)
        else:
            await UAC_CALLS[kind](audio)

    asyncio.run(scenario())
    assert gil_entries(m) == 0


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
def test_a_failed_enumeration_raises_with_its_step_without_the_gil(
    u575: popcorn.MachineDescription, running: bool
) -> None:
    m = usb_machine(u575, UsbHostOptions(timeout=ms(1)), OTG_ELF)
    host = m.periph.OTG_FS.host

    async def scenario() -> None:
        if running:
            async with m.running():
                await host.wait_configured(for_=ms(300))
        else:
            await host.wait_configured(for_=ms(300))

    with pytest.raises(popcorn.UsbTimeout, match="enumeration failed at get_device_head") as caught:
        asyncio.run(scenario())
    assert caught.value.status == "get_device_head"
    assert gil_entries(m) == 0


@pytest.mark.parametrize("running", [False, True], ids=["parked", "running"])
def test_concurrent_control_requests_each_take_their_own_result(
    u575: popcorn.MachineDescription, running: bool
) -> None:
    """Running, the first request takes its result only once the second one's result waits
    too, and the second one starts waiting only after that take."""
    m, host = configured(u575)

    wait, post = host._wait, host._post  # pyright: ignore[reportPrivateUsage]
    first_taken = asyncio.Event()
    second_posted = asyncio.Event()
    second: list[Any] = []
    waits = 0
    first_waited = False
    takes = 0

    async def wait_second_after_first_took(ready: Any, for_: Any, deadline: Any) -> VirtualTime:
        nonlocal waits, first_waited
        waits += 1
        if waits == 1:
            await wait(ready, for_, deadline)
            await second_posted.wait()
            done = await wait(second[0], None, None)
            first_waited = True
            return done
        second.append(ready)
        second_posted.set()
        await first_taken.wait()
        return await wait(ready, for_, deadline)

    def mark_first_take(command: Any, *args: Any, **kwargs: Any) -> Any:
        nonlocal takes
        posted = post(command, *args, **kwargs)
        if first_waited and takes == 0:
            takes += 1

            async def taken() -> Any:
                done = await posted
                first_taken.set()
                return done

            return taken()
        return posted

    if running:
        host._wait = wait_second_after_first_took  # pyright: ignore[reportPrivateUsage]
        host._post = mark_first_take  # pyright: ignore[reportPrivateUsage]

    async def both() -> tuple[UsbData, UsbData]:
        return await asyncio.gather(
            host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 18),
            host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 8),
        )

    async def one_then_the_other() -> tuple[UsbData, UsbData]:
        longer = await host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 18)
        return longer, await host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 8)

    async def scenario() -> tuple[UsbData, UsbData]:
        if running:
            async with m.running():
                return await both()
        return await one_then_the_other()

    longer, shorter = run_bounded(scenario(), HANG)
    assert (len(longer), len(shorter)) == (18, 8)
    assert longer[:8] == shorter


@pytest.mark.parametrize("when", ["post", "in_flight", "ended", "twice"])
def test_a_cancelled_control_request_gives_its_result_up(
    u575: popcorn.MachineDescription, when: str
) -> None:
    """Cancelled after its command ran but before the post returned, while the transfer runs,
    once it ended, and twice while it runs: no result stays behind."""
    m, host = configured(u575)
    post = host._post  # pyright: ignore[reportPrivateUsage]
    wait = host._wait  # pyright: ignore[reportPrivateUsage]
    reached = asyncio.Event()

    async def hang() -> None:
        reached.set()
        await asyncio.Event().wait()

    def post_then_hang(command: Any, *args: Any, **kwargs: Any) -> Any:
        posted = post(command, *args, **kwargs)

        async def held() -> Any:
            await posted
            await hang()

        return held()

    async def hang_instead(ready: Any, for_: Any, deadline: Any) -> VirtualTime:
        await hang()
        raise AssertionError

    async def wait_then_hang(ready: Any, for_: Any, deadline: Any) -> VirtualTime:
        await wait(ready, for_, deadline)
        await hang()
        raise AssertionError

    if when == "post":
        host._post = post_then_hang  # pyright: ignore[reportPrivateUsage]
    elif when == "ended":
        host._wait = wait_then_hang  # pyright: ignore[reportPrivateUsage]
    else:
        host._wait = hang_instead  # pyright: ignore[reportPrivateUsage]

    async def scenario() -> None:
        task = asyncio.create_task(host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 8))
        await reached.wait()
        task.cancel()
        if when == "twice":
            task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        host._post = post  # pyright: ignore[reportPrivateUsage]
        host._wait = wait  # pyright: ignore[reportPrivateUsage]
        await m.run(for_=ms(20))

    run_bounded(scenario(), HANG)
    assert host._link.take_results() == []  # pyright: ignore[reportPrivateUsage]


def test_a_stale_control_forget_after_a_restore_leaves_the_new_request(
    u575: popcorn.MachineDescription,
) -> None:
    """The restore rewinds the transfer ids, so the new request reuses the cancelled one's id."""
    m, host = configured(u575)
    wait = host._wait  # pyright: ignore[reportPrivateUsage]
    saved = m.snapshot()
    reached = asyncio.Event()

    async def hang_instead(ready: Any, for_: Any, deadline: Any) -> VirtualTime:
        reached.set()
        await asyncio.Event().wait()
        raise AssertionError

    async def scenario() -> UsbData:
        host._wait = hang_instead  # pyright: ignore[reportPrivateUsage]
        async with m.running():
            first = asyncio.create_task(host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 8))
            await reached.wait()
        m.restore(saved)

        async def cancel_first_then_wait(ready: Any, for_: Any, deadline: Any) -> VirtualTime:
            first.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await first
            return await wait(ready, for_, deadline)

        host._wait = cancel_first_then_wait  # pyright: ignore[reportPrivateUsage]
        return await host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 18)

    assert len(run_bounded(scenario(), HANG)) == 18


def test_a_control_wait_ended_before_its_result_gives_the_result_up(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    now = m.time

    async def idle(ready: Any, for_: Any, deadline: Any) -> VirtualTime:
        return now

    host._wait = idle  # pyright: ignore[reportPrivateUsage]

    async def scenario() -> None:
        with pytest.raises(popcorn.EmulationError, match="the run ended before control"):
            await host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 8)
        await m.run(for_=ms(20))

    run_bounded(scenario(), HANG)
    assert host._link.take_results() == []  # pyright: ignore[reportPrivateUsage]


class FailOnce(logging.Filter):
    """Raises on the first record it sees, passes the others."""

    def __init__(self) -> None:
        super().__init__()
        self.failed = False

    def filter(self, record: logging.LogRecord) -> bool:
        if not self.failed:
            self.failed = True
            raise RuntimeError("filter failed")
        return True


def test_a_cdc_pty_overflow_warning_reaches_a_raising_filter_off_the_machine_thread(
    u575: popcorn.MachineDescription,
) -> None:
    """The firmware echoes what the pty sends and nothing reads the echo, so the bridge's output
    passes its 64 KiB bound and warns once. Each read of the running machine flushes its log
    records on the test thread: the raising filter runs there, and its error comes out of the
    next read while the free run goes on."""
    m, _ = configured(u575)
    path = m.board.pty("OTG_FS")
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    failing = FailOnce()
    logger = logging.getLogger("popcorn")
    logger.addFilter(failing)
    chunk = bytes(4096)
    raised: list[RuntimeError] = []

    async def scenario() -> bool:
        deadline = time.monotonic() + HANG - 1
        async with m.running():
            while not raised and time.monotonic() < deadline:
                with contextlib.suppress(BlockingIOError):
                    os.write(fd, chunk)
                try:
                    await m.read(m.ref.reg.r0)
                except RuntimeError as error:
                    raised.append(error)
            return m.busy

    try:
        assert run_bounded(scenario(), HANG)
    finally:
        logger.removeFilter(failing)
        os.close(fd)
    assert failing.failed
    assert [str(error) for error in raised] == ["filter failed"]


def gpio_hole(description: popcorn.MachineDescription) -> int:
    """A byte of GPIOA's window no register covers: a read of it logs one record."""
    found = next(p for p in description.peripherals if p.name == "GPIOA")
    taken = {r.offset + k for r in found.registers for k in range(4)}
    return found.base + next(o for o in range(found.size) if o not in taken)


def test_a_log_error_at_a_consuming_await_keeps_its_result(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    hole = gpio_hole(u575)
    failing = FailOnce()
    logger = logging.getLogger("popcorn")

    async def scenario() -> int:
        async with m.running():
            logger.addFilter(failing)
            (value,) = await m.read(m.ref.mem.u8[hole])
            assert failing.failed
            with pytest.raises(RuntimeError, match="filter failed"):
                m.read()
            await m.read()
            reply = await host.control(0x80, GET_DESCRIPTOR, 0x0100, 0, 18)
            assert len(reply) == 18
            return value

    try:
        assert run_bounded(scenario(), HANG) == 0
    finally:
        logger.removeFilter(failing)


def test_forgetting_on_the_links_of_a_dead_machine_does_nothing(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    cdc = usb_link_of(host.cdc(0))
    hid = usb_link_of(host.hid(1))
    del m, host
    gc.collect()
    cdc.forget(1)
    hid.forget(1)
    assert hid.release_written(1) is None


def test_a_dropped_report_write_leaves_no_result_behind(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    hid = host.hid()
    asyncio.run(m.run(for_=ms(20), stop_when=lambda _: hid.report_descriptor != b""))
    writing = hid.write_report(b"\x02\x55")
    del writing
    gc.collect()
    asyncio.run(m.run(for_=ms(20)))
    link = hid._link  # pyright: ignore[reportPrivateUsage]
    assert [link.take_written(write) for write in range(1, 4)] == [None, None, None]


def ready_hid(m: popcorn.Machine, host: UsbHost) -> HidStream:
    hid = host.hid()
    asyncio.run(m.run(for_=ms(20), stop_when=lambda _: hid.report_descriptor != b""))
    return hid


def test_a_dropped_stamped_report_write_leaves_its_machine_collectable(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    hid = ready_hid(m, host)
    writing = hid.write_report(b"\x02\x55", at=m.time + s(100))
    del writing
    gone = weakref.ref(m)
    del m, host, hid
    gc.collect()
    assert gone() is None


def test_a_dropped_report_write_on_a_running_machine_takes_no_gil(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    hid = ready_hid(m, host)
    at = m.time + s(5)
    before = gil_entries(m)

    async def scenario() -> None:
        async with m.running():
            writing = hid.write_report(b"\x02\x55", at=at)
            del writing
            gc.collect()
            later = m._post(None, at + ms(10))  # pyright: ignore[reportPrivateUsage]
            while not later.done():
                await asyncio.sleep(0)
            await later

    run_bounded(scenario(), HANG)
    assert gil_entries(m) == before
    assert m.var("hid_led_writes").value == 1


def test_a_report_write_left_by_a_cancel_warns_of_its_failure_and_keeps_no_result(
    u575: popcorn.MachineDescription, caplog: pytest.LogCaptureFixture
) -> None:
    m = usb_machine(u575, UsbHostOptions(timeout=ms(20)))
    host = m.periph.OTG_FS.host
    asyncio.run(host.wait_configured(for_=s(2)))
    hid = ready_hid(m, host)
    m.periph.OTG_FS.GAHBCFG.GINTMSK = 0
    writing = hid.write_report(b"\x02\x55", at=m.time + ms(1))

    async def cancelled() -> None:
        task = asyncio.create_task(writing)
        await asyncio.sleep(0)
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task

    asyncio.run(cancelled())
    with caplog.at_level(logging.WARNING, logger="popcorn"):
        asyncio.run(m.run(for_=ms(300)))
    ended = [
        record.getMessage() for record in caplog.records if "output report" in record.getMessage()
    ]
    assert len(ended) == 1
    assert "timed_out" in ended[0]
    link = usb_link_of(hid)
    assert [link.take_written(write) for write in range(1, 4)] == [None, None, None]


def test_a_report_write_collected_on_the_machine_thread_warns_off_it(
    u575: popcorn.MachineDescription, caplog: pytest.LogCaptureFixture
) -> None:
    m = usb_machine(u575, UsbHostOptions(timeout=ms(20)))
    host = m.periph.OTG_FS.host
    asyncio.run(host.wait_configured(for_=s(2)))
    hid = ready_hid(m, host)
    m.periph.OTG_FS.GAHBCFG.GINTMSK = 0
    held = [hid.write_report(b"\x02\x55")]
    asyncio.run(m.run(for_=ms(300)))

    def drop(_: object) -> bool:
        held.clear()
        return True

    with caplog.at_level(logging.WARNING, logger="popcorn"):
        asyncio.run(m.run(for_=ms(1), stop_when=drop))
    ended = [record for record in caplog.records if "output report" in record.getMessage()]
    assert [record.thread for record in ended] == [threading.get_ident()]
    assert "timed_out" in ended[0].getMessage()


SAI_ELF = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "sai_ll.elf"

EMPTY_TAKES: dict[str, tuple[Path, Callable[[popcorn.Machine], Any], str]] = {
    "uart": (BOARD_ELF, lambda m: m.periph.USART1.port, "take_frames"),
    "sai": (SAI_ELF, lambda m: m.periph.SAI1.audio("A"), "take_frames"),
    "cdc": (USB_ELF, lambda m: m.periph.OTG_FS.host.cdc(), "take_packets"),
    "hid": (USB_ELF, lambda m: m.periph.OTG_FS.host.hid(), "take_reports"),
    "mic": (AUDIO_ELF, lambda m: m.periph.OTG_FS.host.audio().mic, "take_frames"),
}


@pytest.mark.parametrize("kind", list(EMPTY_TAKES))
def test_a_take_of_a_stream_that_never_held_anything_is_empty_while_running(
    u575: popcorn.MachineDescription, kind: str
) -> None:
    elf, stream_of, method = EMPTY_TAKES[kind]
    if elf in (USB_ELF, AUDIO_ELF):
        m = usb_machine(u575, UsbHostOptions(attached=False), elf)
    else:
        m = popcorn.Machine(u575, image=popcorn.Image.elf(elf))
        m.reset()
    stream = stream_of(m)
    take = getattr(stream, method)

    async def scenario() -> tuple[object, ...]:
        async with m.running():
            await m.elapse(ms(1))
            return take(), stream.available

    assert run_bounded(scenario(), HANG) == ([], 0)


def test_cdc_read_until_takes_through_the_match(u575: popcorn.MachineDescription) -> None:
    m, host = configured(u575)
    cdc = host.cdc()

    async def body() -> tuple[bytes, re.Match[bytes]] | None:
        await cdc.wait_ready(for_=ms(50))
        await cdc.write(b"abc\nrest")
        return await cdc.read_until(re.compile(rb"\n"), for_=ms(50))

    found = asyncio.run(body())
    assert found is not None
    assert found[0] == b"ABC\n"
    assert found[1].end() == 4
    asyncio.run(m.run(for_=ms(5)))
    assert cdc.take() == b"REST"
    m.board.pty("OTG_FS")
    with pytest.raises(RuntimeError, match="bridged"):
        asyncio.run(cdc.read_until(re.compile(rb"x")))
    m.board.close()


def test_cdc_read_until_matches_the_shortest_prefix_of_a_packet(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    cdc = host.cdc()

    async def queued() -> None:
        await cdc.wait_ready(for_=ms(50))
        await cdc.write(b"123x")
        await m.run(for_=ms(5))

    start = m.snapshot()
    asyncio.run(queued())
    assert [p.data for p in cdc.take_packets()] == [b"123X"]
    m.restore(start)
    asyncio.run(queued())
    found = asyncio.run(cdc.read_until(re.compile(rb"\d+"), for_=ms(1)))
    assert found is not None
    assert found[0] == b"1"
    assert cdc.take() == b"23X"


def test_cdc_read_until_reports_bytes_the_full_queue_dropped(
    u575: popcorn.MachineDescription,
) -> None:
    _, host = configured(u575)
    cdc = host.cdc()
    cdc._link.set_byte_limit(2)  # pyright: ignore[reportPrivateUsage]

    async def body() -> tuple[bytes, re.Match[bytes]] | None:
        await cdc.wait_ready(for_=ms(50))
        await cdc.write(b"abcd")
        return await cdc.read_until(re.compile(rb"z"), for_=ms(20))

    with pytest.raises(RuntimeError, match="dropped bytes"):
        asyncio.run(body())
    assert cdc.available <= 2


def test_cdc_take_equals_a_read_of_what_is_available(u575: popcorn.MachineDescription) -> None:
    m, host = configured(u575)
    cdc = host.cdc()
    start = m.snapshot()

    async def exchanged() -> None:
        await cdc.write(b"abc")
        await m.run(for_=ms(20))

    asyncio.run(exchanged())
    taken = cdc.take_packets()
    assert (cdc.available, cdc.take(), cdc.take_packets()) == (0, b"", [])
    m.restore(start)
    asyncio.run(exchanged())
    assert taken == asyncio.run(cdc.read_packets(len(taken)))
    assert b"".join(p.data for p in taken) == b"ABC"
    m.restore(start)
    asyncio.run(exchanged())
    assert cdc.take() == b"ABC"


def test_cdc_take_inside_running_empties_the_queue(u575: popcorn.MachineDescription) -> None:
    m, host = configured(u575)
    cdc = host.cdc()

    async def body() -> bytes:
        async with m.running():
            await cdc.write(b"abc")
            while cdc.available < 3:
                await m.elapse(ms(1))
            return cdc.take()

    assert run_bounded(body(), HANG) == b"ABC"
    assert cdc.available == 0


CDC_SENT = bytes(ord("a") + i % 26 for i in range(2000))


async def cdc_bytes_read(cdc: Any, at: VirtualTime) -> tuple[bytes, bytes]:
    first: bytes = await cdc.read(4000, deadline=at + ms(8))
    return first, await cdc.read(4000, deadline=at + ms(60))


async def cdc_packets_read(cdc: Any, at: VirtualTime) -> tuple[bytes, bytes]:
    first: list[CdcPacket] = await cdc.read_packets(100, deadline=at + ms(8))
    later: list[CdcPacket] = await cdc.read_packets(100, deadline=at + ms(60))
    assert first[-1].at <= at + ms(8) < later[0].at
    return b"".join(p.data for p in first), b"".join(p.data for p in later)


@pytest.mark.parametrize("reads", [cdc_bytes_read, cdc_packets_read])
def test_a_free_run_cdc_deadline_read_takes_what_a_parked_one_takes(
    u575: popcorn.MachineDescription,
    reads: Callable[[Any, VirtualTime], Coroutine[Any, Any, tuple[bytes, bytes]]],
) -> None:
    m, host = configured(u575)
    cdc = host.cdc()
    asyncio.run(cdc.wait_ready(for_=ms(50)))
    start = m.snapshot()

    def post(at: VirtualTime) -> None:
        cdc.write(CDC_SENT, at=at + ms(1))

    async def running() -> tuple[VirtualTime, tuple[bytes, bytes]]:
        async with m.running():
            return await late_drained_read(m, post, lambda at: reads(cdc, at))

    at, (first, later) = run_bounded(running(), HANG)
    assert first
    assert later
    assert first + later == CDC_SENT.upper()
    m.restore(start)
    post(at)
    assert asyncio.run(reads(cdc, at)) == (first, later)


def test_a_bridged_cdc_function_is_not_taken_from_python(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    m.board.pty("OTG_FS")
    with pytest.raises(RuntimeError, match="bridged"):
        host.cdc().take()
    with pytest.raises(RuntimeError, match="bridged"):
        host.cdc().take_packets()
    m.board.close()


def test_hid_take_reports_equals_a_read_of_what_is_available(
    u575: popcorn.MachineDescription,
) -> None:
    m, host = configured(u575)
    echo = host.hid(1)
    asyncio.run(echo.wait_ready(for_=ms(50)))
    start = m.snapshot()

    async def exchanged() -> None:
        await echo.write_report(b"\x00\x0f")
        await m.run(for_=ms(20))

    asyncio.run(exchanged())
    taken = echo.take_reports()
    assert [r.data for r in taken] == [b"\xf0"]
    assert (echo.available, echo.take_reports()) == (0, [])
    m.restore(start)
    asyncio.run(exchanged())
    assert taken == asyncio.run(echo.read_reports(echo.available))


def test_mic_take_frames_equals_a_read_of_what_is_available(
    u575: popcorn.MachineDescription,
) -> None:
    m = audio_machine(u575)
    audio = m.periph.OTG_FS.host.audio()

    async def started() -> None:
        await audio.wait_ready(for_=s(2))
        await audio.mic.start()

    asyncio.run(started())
    start = m.snapshot()
    asyncio.run(m.run(for_=ms(5)))
    taken = audio.mic.take_frames()
    assert len(taken) >= 4
    assert (audio.mic.available, audio.mic.take_frames()) == (0, [])
    m.restore(start)
    asyncio.run(m.run(for_=ms(5)))
    assert taken == asyncio.run(audio.mic.read_frames(audio.mic.available))


type Bounded = Callable[[Any, dict[str, Any]], Coroutine[Any, Any, object]]

USB_WAITS: dict[str, tuple[Path, Callable[[UsbHost], Any], Bounded]] = {
    "cdc-read": (USB_ELF, UsbHost.cdc, lambda cdc, bound: cdc.read(1, **bound)),
    "cdc-read-packets": (USB_ELF, UsbHost.cdc, lambda cdc, bound: cdc.read_packets(1, **bound)),
    "cdc-ready": (USB_ELF, UsbHost.cdc, lambda cdc, bound: cdc.wait_ready(**bound)),
    "hid-ready": (USB_ELF, UsbHost.hid, lambda hid, bound: hid.wait_ready(**bound)),
    "hid-read-reports": (USB_ELF, UsbHost.hid, lambda hid, bound: hid.read_reports(1, **bound)),
    "configured": (USB_ELF, lambda host: host, lambda host, bound: host.wait_configured(**bound)),
    "audio-ready": (AUDIO_ELF, UsbHost.audio, lambda audio, bound: audio.wait_ready(**bound)),
    "speaker-drain": (
        AUDIO_ELF,
        lambda host: host.audio().speaker,
        lambda speaker, bound: speaker.drain(**bound),
    ),
    "mic-read-frames": (
        AUDIO_ELF,
        lambda host: host.audio().mic,
        lambda mic, bound: mic.read_frames(1, **bound),
    ),
}


@pytest.mark.parametrize("kind", list(USB_WAITS))
def test_a_usb_wait_with_a_past_deadline_runs_nothing(
    u575: popcorn.MachineDescription, kind: str
) -> None:
    elf, stream_of, wait = USB_WAITS[kind]
    m = usb_machine(u575, elf=elf)
    asyncio.run(m.run(for_=ms(1)))
    stream = stream_of(m.periph.OTG_FS.host)
    before = m.instructions
    with contextlib.suppress(popcorn.UsbTimeout):
        assert asyncio.run(wait(stream, {"deadline": VirtualTime(0)})) in (None, b"", [])
    assert m.instructions == before


@pytest.mark.parametrize("kind", list(USB_WAITS))
def test_a_usb_wait_with_a_past_deadline_ends_at_once_while_running(
    u575: popcorn.MachineDescription, kind: str
) -> None:
    elf, stream_of, wait = USB_WAITS[kind]
    m = usb_machine(u575, UsbHostOptions(attached=False), elf)
    stream = stream_of(m.periph.OTG_FS.host)

    async def scenario() -> object:
        async with m.running():
            past = await m.now()
            with contextlib.suppress(popcorn.UsbTimeout):
                return await wait(stream, {"deadline": past})
        return None

    assert run_bounded(scenario(), HANG) in (None, b"", [])


@pytest.mark.parametrize("kind", [kind for kind in USB_WAITS if kind != "speaker-drain"])
def test_a_usb_wait_with_a_deadline_stops_there_while_running(
    u575: popcorn.MachineDescription, kind: str
) -> None:
    elf, stream_of, wait = USB_WAITS[kind]
    m = usb_machine(u575, UsbHostOptions(attached=False), elf)
    stream = stream_of(m.periph.OTG_FS.host)

    async def scenario() -> tuple[VirtualTime, VirtualTime]:
        async with m.running():
            start = await m.now()
            with contextlib.suppress(popcorn.UsbTimeout):
                await wait(stream, {"deadline": start + ms(2)})
            return start, await m.now()

    start, after = run_bounded(scenario(), HANG)
    assert after >= start + ms(2)


@pytest.mark.parametrize("kind", list(USB_WAITS))
def test_a_usb_wait_refuses_both_for_and_deadline(
    u575: popcorn.MachineDescription, kind: str
) -> None:
    elf, stream_of, wait = USB_WAITS[kind]
    m = usb_machine(u575, elf=elf)
    stream = stream_of(m.periph.OTG_FS.host)
    before = m.instructions
    with pytest.raises(TypeError, match="exclusive"):
        asyncio.run(wait(stream, {"for_": ms(1), "deadline": m.time}))
    assert m.instructions == before
