"""The virtual USB host of OTG_FS from Python: wiring, cable, enumeration state, control
requests, snapshots and the project file.

The firmware is the usb_cdc example's: TinyUSB 0.21.0, a CDC ACM plus HID composite device."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
import pytest
from popcorn import _core
from popcorn.board import UsbHost, UsbHostOptions, UsbState
from popcorn.units import ms, s

pytest_plugins = ["pytester"]

ROOT = Path(__file__).parents[3]
USB_ELF = ROOT / "examples" / "stm32u575" / "usb_cdc" / "prebuilt" / "usb_cdc.elf"
OTG_ELF = ROOT / "examples" / "stm32u575" / "otg_fs" / "prebuilt" / "otg_fs_init.elf"
BUDGET = 5_000_000

GET_DESCRIPTOR = 6
SET_LINE_CODING = 0x20
GET_LINE_CODING = 0x21
LINE_CODING = bytes([0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08])


def usb_machine(
    u575: popcorn.MachineDescription, options: UsbHostOptions | None = None
) -> popcorn.Machine:
    m = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(USB_ELF),
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
    assert [i.number for i in host.interfaces] == [0, 1, 2]
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


def project(tmp_path: Path, board: str) -> popcorn.Project:
    path = tmp_path / "popcorn.toml"
    path.write_text(
        f'[mcu]\npart = "STM32U575QGIx"\n[image.app]\nelf = "{USB_ELF}"\n'
        f'[boot]\nuntil = "main"\nbudget = {BUDGET}\n{board}'
    )
    return popcorn.Project.load(path)


def test_a_project_file_wires_the_host_and_its_timings(
    u575: popcorn.MachineDescription, tmp_path: Path
) -> None:
    del u575
    board = "[board.OTG_FS]\nhost = { attached = false, debounce_ms = 20 }\n"
    m = project(tmp_path, board).machine()
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
        "[board.USART1]\nhost = {}\n",
        f'[models]\npython = ["{Path(__file__).parent}"]\n'
        '[board.OTG_FS]\nmodel = "test_audio:Silent"\nhost = {}\n',
    ],
)
def test_a_host_table_needs_a_usb_controller_with_the_built_in_model(
    u575: popcorn.MachineDescription, tmp_path: Path, board: str
) -> None:
    del u575
    with pytest.raises(popcorn.ConfigError) as caught:
        project(tmp_path, board).machine()
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
    pytester.makeconftest('pytest_plugins = ["popcorn.pytest"]\n')
    (pytester.path / "popcorn.toml").write_text(
        f'[mcu]\npart = "STM32U575QGIx"\ndescription = "{u575_json}"\n'
        f'[image.app]\nelf = "{USB_ELF}"\n'
        f'[boot]\nuntil = "usb_configured"\nbudget = {BUDGET}\n[board.OTG_FS]\nhost = {{}}\n'
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
