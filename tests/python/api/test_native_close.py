"""`MachineHandle.close()`: every object made from a machine refuses its use with EmulationError
once the machine is closed, and the machine's thread and models are gone while it is held."""

from __future__ import annotations

import asyncio
import gc
import sys
import threading
import weakref
from collections.abc import Callable
from typing import Any

import popcorn
import pytest
from popcorn import _core
from popcorn._hooks import Relay
from popcorn.units import ms, s

from .conftest import BOARD_ELF, M33_JSON, collected, holds_pty, quiet_host_threads, settled
from .test_audio import sai_machine
from .test_intercepts import call
from .test_models import Timer, timer_of
from .test_models import machine as model_machine
from .test_pdm import machine as pdm_machine
from .test_usb import ORPHAN_OPS, Orphans, audio_machine, configured, usb_link_of

CLOSED = "the machine was closed"


def refused(use: Callable[[], Any]) -> None:
    with pytest.raises(popcorn.EmulationError, match=CLOSED):
        use()


def test_a_held_uart_stream_refuses_every_use_after_close(board: popcorn.Machine) -> None:
    uart = board.periph.USART1.port
    before = quiet_host_threads()
    board.close()
    assert _core.host_threads() <= before
    assert uart.available == 0
    refused(lambda: uart.queued)
    refused(lambda: asyncio.run(uart.write(b"x")))
    refused(lambda: asyncio.run(uart.read_frames(1, for_=ms(1))))


def test_a_held_gpio_pin_refuses_every_use_after_close(board: popcorn.Machine) -> None:
    pin = board.periph.GPIOA.pin(0)
    board.close()
    refused(lambda: pin.level)
    refused(lambda: pin.mode)
    refused(pin.release)


def test_held_views_registers_and_memory_refuse_every_use_after_close(
    board: popcorn.Machine,
) -> None:
    mem, reg, periph = board.mem, board.reg, board.periph
    board.close()
    refused(lambda: mem.u32[0x2000_0000])
    refused(lambda: mem.read(0x2000_0000, 4))
    refused(lambda: reg.pc)
    refused(lambda: periph.USART1.CR2.read())


def test_a_held_audio_stream_refuses_every_use_after_close(u575: popcorn.PartDescription) -> None:
    m = sai_machine(u575)
    stream = m.periph.SAI1.audio("A")
    m.close()
    refused(lambda: stream.available)
    refused(lambda: asyncio.run(stream.read_frames(1, for_=ms(1))))


def test_a_held_pdm_microphone_refuses_every_use_after_close(u575: popcorn.PartDescription) -> None:
    m = pdm_machine(u575)
    mic = m.periph.MDF1.mic(0, "rising")
    m.close()
    refused(lambda: asyncio.run(mic.write_pdm(b"\x00", bits=8)))


def test_a_pty_bridge_is_closed_with_its_machine(board: popcorn.Machine) -> None:
    path = board.board.pty("USART1")
    native = board._handle  # pyright: ignore[reportPrivateUsage]
    assert holds_pty(path)
    board.close()
    assert settled(lambda: not holds_pty(path))
    assert native.closed()


def test_a_held_virtual_loop_refuses_every_use_after_close(board: popcorn.Machine) -> None:
    loop = board.loop
    board.close()
    with pytest.raises(RuntimeError, match="Event loop is closed"):
        loop.call_later(1.0, lambda: None)


def test_the_removal_of_a_hook_after_close_does_nothing(hooked: popcorn.Machine) -> None:
    def handler(call: popcorn.Call) -> popcorn.Return:
        return call.returns()

    probe = weakref.ref(handler)
    installed = hooked.intercept("hal_delay", handler)
    del handler
    with hooked.intercepted("note", lambda c: c.proceed()) as scoped:
        hooked.close()
    assert not scoped.active
    installed.remove()
    installed.remove()
    assert not installed.active
    gc.collect()
    assert collected(probe)


@pytest.mark.parametrize("first", ["view", "machine"])
def test_dropping_the_holders_of_a_closed_machine_in_any_order_reports_nothing(
    board: popcorn.Machine, monkeypatch: pytest.MonkeyPatch, first: str
) -> None:
    reported: list[str | None] = []

    def report(raised: sys.UnraisableHookArgs) -> None:
        reported.append(raised.err_msg)

    monkeypatch.setattr(sys, "unraisablehook", report)
    native = board._handle  # pyright: ignore[reportPrivateUsage]
    view = board.mem
    board.close()
    if first == "view":
        del view, board
        gc.collect()
        del native
    else:
        del board
        gc.collect()
        del native
        gc.collect()
        del view
    gc.collect()
    assert reported == []


def test_a_held_usb_link_refuses_every_use_after_close(u575: popcorn.PartDescription) -> None:
    m, host = configured(u575)
    usb = usb_link_of(host)
    cdc, hid, ticket = usb.cdc(0), usb.hid(0), usb.ticket()
    audio = audio_machine(u575)
    audio_host = audio.periph.OTG_FS.host
    asyncio.run(audio_host.wait_configured(for_=s(2)))
    uac = usb_link_of(audio_host).audio(0)
    links = Orphans(cdc, hid, usb, uac, ticket)
    m.close()
    audio.close()
    for name, op in ORPHAN_OPS.items():
        with pytest.raises(popcorn.EmulationError, match=CLOSED):
            op(links)
        assert name


def test_closing_from_a_callback_of_another_machine_is_refused_and_the_machine_survives(
    board: popcorn.Machine, hooked: popcorn.Machine
) -> None:
    errors: list[BaseException] = []

    def closer(call: popcorn.Call) -> popcorn.Return:
        try:
            board.close()
        except popcorn.MachineRunning as raised:
            errors.append(raised)
        return call.returns()

    hooked.intercept("hal_delay", closer)
    call(hooked, "use_delay")
    assert len(errors) == 1
    assert board.periph.USART1.port.queued == 0


def test_a_machine_a_system_handle_claimed_is_not_closed_until_the_system_is_gone(
    u575: popcorn.PartDescription,
) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
    m.reset()
    native = m._handle  # pyright: ignore[reportPrivateUsage]
    system = _core.SystemHandle(1000, 1_000_000)
    system.add_member(native, "m", False)
    with pytest.raises(popcorn.ConfigError, match="members"):
        native.close(False)
    assert not native.closed()
    assert m.mem.u32[0x2000_0000] is not None
    del system
    gc.collect()
    m.close()
    assert native.closed()


def test_a_closed_machine_cannot_become_a_system_member(u575: popcorn.PartDescription) -> None:
    m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
    native = m._handle  # pyright: ignore[reportPrivateUsage]
    m.close()
    system = _core.SystemHandle(1000, 1_000_000)
    with pytest.raises(popcorn.EmulationError, match=CLOSED):
        system.add_member(native, "m", False)


def test_the_native_close_refuses_a_host_thread_and_the_machine_survives(
    board: popcorn.Machine, hooked: popcorn.Machine
) -> None:
    native = board._handle  # pyright: ignore[reportPrivateUsage]
    errors: list[BaseException] = []

    def closer(call: popcorn.Call) -> popcorn.Return:
        try:
            native.close(False)
        except popcorn.MachineRunning as raised:
            errors.append(raised)
        return call.returns()

    hooked.intercept("hal_delay", closer)
    call(hooked, "use_delay")
    assert len(errors) == 1
    assert not native.closed()
    assert board.periph.USART1.port.queued == 0


def test_a_held_predicate_refuses_holds_after_close(u575: popcorn.PartDescription) -> None:
    native = _core.MachineHandle(u575, _core.MachineConfig())
    link = native.attach_uart("USART1")
    predicate = link.predicate_available(1)
    assert not predicate.holds()
    native.close(False)
    assert native.closed()
    refused(predicate.holds)


def test_closing_many_machines_leaves_no_registry_entry(u575: popcorn.PartDescription) -> None:
    before = _core.registered_hosts()
    for _ in range(10):
        m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
        m.reset()
        asyncio.run(m.run(budget=1_000))
        m.close()
    assert _core.registered_hosts() <= before


def test_a_held_native_loop_port_refuses_every_use_after_close(board: popcorn.Machine) -> None:
    table = board._callback_table  # pyright: ignore[reportPrivateUsage]
    key = table.add(lambda *args: None, in_caller=True)
    native = board._handle  # pyright: ignore[reportPrivateUsage]
    port = native.virtual_loop_port_of(Relay(table, key, quiet=True))
    assert port.board_time_ps() >= 0
    board.close()
    refused(lambda: port.arm(1))
    refused(port.disarm)
    refused(port.board_time_ps)
    refused(lambda: port.command_post(1, 1))


def test_a_held_model_context_refuses_every_use_after_close() -> None:
    m = model_machine({"TIMER": Timer})
    timer = timer_of(m)
    assert timer.now.ps >= 0
    m.close()
    refused(lambda: timer.now)


def test_a_held_pin_watch_is_inactive_and_removes_silently_after_close(
    board: popcorn.Machine,
) -> None:
    table = board._callback_table  # pyright: ignore[reportPrivateUsage]
    key = table.add(lambda *args: None, in_caller=True)
    native = board._handle  # pyright: ignore[reportPrivateUsage]
    watch = native.gpio_watch("GPIOA", 2, Relay(table, key, quiet=True))
    assert watch.active()
    board.close()
    assert not watch.active()
    watch.remove()
    watch.remove()


def test_a_held_uart_device_link_refuses_flow_control_and_sends_after_close(
    board: popcorn.Machine,
) -> None:
    link = board._handle.uart_device_link_of("USART1")  # pyright: ignore[reportPrivateUsage]
    assert isinstance(link.has_flow_control(), bool)
    board.close()
    refused(link.has_flow_control)
    refused(link.rts_ready)
    refused(lambda: link.send(b"x", 0, False, 0, 0, 0))
    refused(lambda: link.set_cts(True, 0))


def test_frames_captured_before_a_close_stay_readable_and_writes_raise(
    board: popcorn.Machine,
) -> None:
    uart = board.periph.USART1.port

    async def echo() -> None:
        await uart.write(b"hello")
        await board.run(for_=ms(1))

    asyncio.run(echo())
    link = uart._link  # pyright: ignore[reportPrivateUsage]
    assert link.available() == 5
    board.close()
    assert link.available() == 5
    assert link.peek(0, 5) == b"hello"
    assert bytes(frame.data for frame in link.drain(5)) == b"hello"
    assert link.available() == 0
    refused(lambda: link.send(b"x"))
    refused(link.queued)
    refused(link.rts_ready)


def race(native: Any, system: Any) -> dict[str, str]:
    start = threading.Barrier(2)
    results: dict[str, str] = {}

    def claim() -> None:
        start.wait()
        try:
            system.add_member(native, "m", False)
            results["claim"] = "claimed"
        except popcorn.EmulationError:
            results["claim"] = "refused"

    def close() -> None:
        start.wait()
        try:
            native.close(False)
            results["close"] = "closed"
        except popcorn.ConfigError:
            results["close"] = "refused"

    threads = [threading.Thread(target=claim), threading.Thread(target=close)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(10)
        assert not thread.is_alive()
    return results


def test_a_system_claim_and_a_close_never_leave_a_claimed_closed_machine() -> None:
    description = popcorn.load_description(str(M33_JSON))
    for _ in range(20):
        native = _core.MachineHandle(description, _core.MachineConfig())
        system = _core.SystemHandle(1000, 1_000_000)
        results = race(native, system)
        assert results["claim"] != "claimed" or results["close"] == "refused"
        assert results["claim"] != "claimed" or not native.closed()
        del system
        native.close(False)
        assert native.closed()


def test_a_closed_native_handle_refuses_the_methods_that_read_the_engine_directly(
    board: popcorn.Machine,
) -> None:
    native = board._handle  # pyright: ignore[reportPrivateUsage]
    board.close()
    refused(lambda: native.model("USART1"))
    refused(native.pc)
    refused(lambda: native.locate(0))
    refused(native.image_digests)


def test_a_collection_after_close_does_not_visit_the_released_models() -> None:
    m = model_machine({"TIMER": Timer})
    native = m._handle  # pyright: ignore[reportPrivateUsage]
    gone = weakref.ref(timer_of(m))
    m.close()
    del m
    for _ in range(3):
        gc.collect()
    assert collected(gone)
    assert native.closed()
    assert gc.get_referents(native) == [type(native)]


def test_a_refused_claim_of_a_closed_machine_leaves_it_unclaimed(
    u575: popcorn.PartDescription,
) -> None:
    native = _core.MachineHandle(u575, _core.MachineConfig())
    system = _core.SystemHandle(1000, 1_000_000)
    native.close(False)
    refused(lambda: system.add_member(native, "m", False))
    assert not native.claimed()
    other = _core.MachineHandle(u575, _core.MachineConfig())
    system.add_member(other, "o", False)
    assert other.claimed()
    with pytest.raises(popcorn.ConfigError, match="members"):
        other.close(False)
