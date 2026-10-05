"""The virtual USB host of OTG_FS from Python: wiring, cable, enumeration state, control
requests, snapshots, the project, the CDC ACM stream, the HID streams and the USB Audio
2.0 function.

The firmware is the usb_cdc example's: TinyUSB 0.21.0, a CDC ACM plus HID composite device. The
audio tests run the usb_audio example's: a TinyUSB 0.21.0 UAC2 headset, 16 kHz mono 16-bit."""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Callable, Coroutine, Mapping
from pathlib import Path
from typing import Any

import popcorn
import pytest
from popcorn import _core
from popcorn.board import CdcLineCoding, HidStream, UsbHost, UsbHostOptions, UsbState
from popcorn.units import VirtualTime, ms, s, us

from .projects import Silent

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
    audio.speaker.stop()
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
        async with m.running():
            draining = asyncio.create_task(speaker.drain())
            await asyncio.sleep(0)
            assert speaker.sending
            if drop == "stop":
                speaker.stop()
            else:
                await host.detach()
            with pytest.raises(popcorn.UsbError) as caught:
                await draining
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
        async with m.running():
            draining = asyncio.create_task(speaker.drain())
            await asyncio.sleep(0)
            speaker.stop()
            with pytest.raises(popcorn.UsbError) as caught:
                await draining
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
            audio.mic.stop()
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
    audio.mic.stop()
    assert not audio.mic.running
    m.restore(saved)
    assert audio.mic.available == 0
    assert audio.mic.dropped == 0
    assert audio.mic.running
    frames = asyncio.run(audio.mic.read_frames(3, for_=ms(5)))
    assert len(frames) == 3
