"""UARTs bridged to a host pty, FR-PER-6."""

from __future__ import annotations

import asyncio
import gc
import logging
import os
import select
import selectors
from pathlib import Path
from typing import cast

import popcorn
import pytest
from popcorn import _core
from popcorn._streams import OUTGOING_LIMIT, PtyBridge
from popcorn.units import ms

from .conftest import BOARD_ELF


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


def test_host_input_with_no_run_logs_a_deadlock_warning(
    board: popcorn.Machine, caplog: pytest.LogCaptureFixture
) -> None:
    async def body() -> str:
        path = board.board.pty("USART1", deadlock_warning=0.05)
        fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        try:
            os.write(fd, b"x")
            await asyncio.sleep(0.3)
        finally:
            os.close(fd)
        return path

    with caplog.at_level(logging.WARNING, logger="popcorn"):
        path = asyncio.run(body())
    warnings = [r.getMessage() for r in caplog.records if r.levelno == logging.WARNING]
    assert len(warnings) == 1 and path in warnings[0] and "USART1" in warnings[0]


def test_a_run_between_slices_moves_pty_bytes_both_ways(
    board: popcorn.Machine, caplog: pytest.LogCaptureFixture
) -> None:
    async def body() -> tuple[bytes, bytes]:
        path = board.board.pty("USART1", deadlock_warning=0.1)
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
        before = len(selector.get_map())
        m = popcorn.Machine(u575, image=popcorn.Image.elf(BOARD_ELF))
        path = m.board.pty("USART1")
        await m.run(for_=ms(1))
        watching = len(selector.get_map())
        del m
        gc.collect()
        assert len(selector.get_map()) == before
        return path, os.path.exists(path), before, watching

    path, exists, before, watching = asyncio.run(
        body(), loop_factory=lambda: asyncio.SelectorEventLoop(selector)
    )
    assert not exists, path
    assert watching == before + 1


def test_close_removes_every_pty(board: popcorn.Machine) -> None:
    path = board.board.pty("USART1")
    board.board.close()
    assert board.board.ptys == {}
    assert not os.path.exists(path)


class FakeLink:
    """A native link stand-in whose firmware transmits `pending` bytes."""

    def __init__(self, pending: bytes) -> None:
        self.pending = [_Frame(b) for b in pending]

    def available(self) -> int:
        return len(self.pending)

    def drain(self, count: int) -> list[_Frame]:
        taken, self.pending = self.pending[:count], self.pending[count:]
        return taken

    def send(self, data: bytes) -> None:
        pass


class _Frame:
    def __init__(self, data: int) -> None:
        self.data = data
        self.at_ps = 0


def test_unread_pty_output_keeps_the_last_64_kib(caplog: pytest.LogCaptureFixture) -> None:
    sent = bytes(range(256)) * 1024
    link = FakeLink(sent[: len(sent) // 2])
    bridge = PtyBridge("USART9", cast(_core.UartLink, link), 1.0)
    try:
        with caplog.at_level(logging.WARNING, logger="popcorn"):
            bridge.pump()
            link.pending = [_Frame(b) for b in sent[len(sent) // 2 :]]
            bridge.pump()
        warnings = [r.getMessage() for r in caplog.records if r.levelno == logging.WARNING]
        assert len(warnings) == 1 and bridge.path in warnings[0]
        fd = os.open(bridge.path, os.O_RDWR | os.O_NOCTTY)
        try:
            received = b""
            while select.select([fd], [], [], 0.2)[0]:
                received += os.read(fd, 65536)
                bridge.pump()
        finally:
            os.close(fd)
        assert received[-4096:] == sent[-4096:]
        assert OUTGOING_LIMIT <= len(received) < 2 * OUTGOING_LIMIT
        assert received == sent[-len(received) :]
    finally:
        bridge.close()
    assert not os.path.exists(bridge.path)


def test_a_bridged_uart_is_not_read_from_python(board: popcorn.Machine) -> None:
    board.board.pty("USART1")
    with pytest.raises(RuntimeError, match="bridged"):
        asyncio.run(board.periph.USART1.port.read(1))


def test_the_project_file_connects_a_pty(tmp_path: Path, u575: popcorn.MachineDescription) -> None:
    text = (
        f'[mcu]\npart = "STM32U575QGIx"\n[image.app]\nelf = "{BOARD_ELF}"\n'
        '[board.USART1]\nconnect = "pty"\n'
    )
    path = tmp_path / "popcorn.toml"
    path.write_text(text)
    m = popcorn.Project.load(path).machine()
    assert set(m.board.ptys) == {"USART1"}
    assert os.path.exists(m.board.ptys["USART1"])


def test_other_connections_and_devices_are_refused(
    tmp_path: Path, u575: popcorn.MachineDescription
) -> None:
    head = f'[mcu]\npart = "STM32U575QGIx"\n[image.app]\nelf = "{BOARD_ELF}"\n'
    path = tmp_path / "popcorn.toml"
    path.write_text(head + '[board.USART1]\nconnect = "socket"\n')
    with pytest.raises(popcorn.ConfigError) as caught:
        popcorn.Project.load(path).machine()
    assert caught.value.key == "board.USART1.connect"
    path.write_text(
        head + '[board.I2C1]\ndevices = [{ address = 0x50, model = "fractions:Fraction" }]\n'
    )
    with pytest.raises(popcorn.ConfigError) as refused:
        popcorn.Project.load(path).machine()
    assert refused.value.key == "board.I2C1.devices[0].model"
