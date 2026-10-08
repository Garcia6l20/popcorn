"""UARTs bridged to a host pty, FR-PER-6."""

from __future__ import annotations

import asyncio
import gc
import logging
import os
import select
import selectors
import time
from fractions import Fraction
from typing import Any, Literal, cast

import popcorn
import pytest
from popcorn import _host
from popcorn.units import ms

from .conftest import BOARD_ELF, holds_pty


def client(path: str, line: bytes) -> bytes:
    """What a host tool does: open the pty, write a line, read the echo."""
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    try:
        os.write(fd, line)
        echoed = b""
        while len(echoed) < len(line):
            ready, _, _ = select.select([fd], [], [], 5.0)
            if not ready:
                break
            echoed += os.read(fd, 64)
        return echoed
    finally:
        os.close(fd)


def test_a_host_tool_talks_to_the_firmware_through_the_pty(board: popcorn.Machine) -> None:
    async def body() -> bytes:
        path = board.board.pty("USART1")
        async with board.running():
            return await asyncio.to_thread(client, path, b"ping\n")

    assert asyncio.run(body()) == b"ping\n"
    assert board.board.ptys == {"USART1": board.board.pty("USART1")}


def test_the_deadlock_warning_argument_is_gone(board: popcorn.Machine) -> None:
    pty: Any = board.board.pty
    with pytest.raises(TypeError, match="deadlock_warning"):
        pty("USART1", deadlock_warning=0.05)
    assert board.board.ptys == {}


def test_a_sync_body_blocked_on_the_pty_gets_the_echo_of_a_running_machine(
    board: popcorn.Machine,
) -> None:
    path = board.board.pty("USART1")
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    board.start()
    try:
        echoed = b""
        for byte in b"sync\n":
            sent = time.monotonic()
            os.write(fd, bytes([byte]))
            ready, _, _ = select.select([fd], [], [], 5.0)
            assert ready, echoed
            echoed += os.read(fd, 64)
            assert time.monotonic() - sent < 1.0
    finally:
        os.close(fd)
        board.halt()
    assert echoed == b"sync\n"


def test_a_run_between_slices_moves_pty_bytes_both_ways(
    board: popcorn.Machine, caplog: pytest.LogCaptureFixture
) -> None:
    async def body() -> tuple[bytes, bytes]:
        path = board.board.pty("USART1")
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        try:
            os.write(fd, b"ab")
            await asyncio.sleep(0.02)
            await board.run(for_=ms(2))
            await asyncio.sleep(0.2)
            await board.run(for_=ms(2))
            echoed = os.read(fd, 64) if select.select([fd], [], [], 1.0)[0] else b""
            extra = os.read(fd, 64) if select.select([fd], [], [], 0.05)[0] else b""
            return echoed, extra
        finally:
            os.close(fd)

    with caplog.at_level(logging.WARNING, logger="popcorn"):
        assert asyncio.run(body()) == (b"ab", b"")
    assert not [r for r in caplog.records if r.levelno == logging.WARNING]


def test_a_pty_made_inside_a_loop_is_freed_with_its_machine(
    u575: popcorn.MachineDescription,
) -> None:
    selector = selectors.DefaultSelector()

    async def body() -> tuple[str, bool, int, int]:
        _host._waker_of(asyncio.get_running_loop())  # pyright: ignore[reportPrivateUsage]
        before = len(selector.get_map())
        m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
        path = m.board.pty("USART1")
        await m.run(for_=ms(1))
        watching = len(selector.get_map())
        del m
        gc.collect()
        assert len(selector.get_map()) == before
        return path, holds_pty(path), before, watching

    path, held, before, watching = asyncio.run(
        body(), loop_factory=lambda: asyncio.SelectorEventLoop(selector)
    )
    assert not held, path
    assert watching == before


def test_close_removes_every_pty(board: popcorn.Machine) -> None:
    path = board.board.pty("USART1")
    board.board.close()
    assert board.board.ptys == {}
    assert not holds_pty(path)


def test_a_bridged_uart_is_not_read_from_python(board: popcorn.Machine) -> None:
    board.board.pty("USART1")
    with pytest.raises(RuntimeError, match="bridged"):
        asyncio.run(board.periph.USART1.port.read(1))


def test_the_project_connects_a_pty(u575: popcorn.MachineDescription) -> None:
    project = popcorn.Project(
        part="STM32U575QGIx",
        images=[popcorn.Elf(BOARD_ELF)],
        board={"USART1": popcorn.Periph(connect="pty")},
    )
    m = project.machine()
    assert set(m.board.ptys) == {"USART1"}
    assert os.path.exists(m.board.ptys["USART1"])


def test_other_connections_and_devices_are_refused(u575: popcorn.MachineDescription) -> None:
    socket = popcorn.Project(
        part="STM32U575QGIx",
        images=[popcorn.Elf(BOARD_ELF)],
        board={"USART1": popcorn.Periph(connect=cast("Literal['pty']", "socket"))},
    )
    with pytest.raises(popcorn.ConfigError) as caught:
        socket.machine()
    assert caught.value.key == "board.USART1.connect"
    fraction = popcorn.Project(
        part="STM32U575QGIx",
        images=[popcorn.Elf(BOARD_ELF)],
        board={"I2C1": popcorn.Periph(devices=[popcorn.Device(Fraction, address=0x50)])},
    )
    with pytest.raises(popcorn.ConfigError) as refused:
        fraction.machine()
    assert refused.value.key == "board.I2C1.devices[0].model"
