"""A generic H4-like peer for the UART device port: plain asyncio, a task per link, timers of
100 to 200 ms, a reset pin and a strap pin, RTS and CTS pacing. Shared by `test_uart_peer.py`."""

from __future__ import annotations

import asyncio
import json
from typing import cast

from popcorn.board import UartDevice
from popcorn.units import VirtualTime

PS_PER_SECOND = 10**12
RESET_PIN = "PA1"
STRAP_PIN = "PA2"
LATENCY_S = 100e-6
ACK_TIMEOUT_S = 0.15
CTS_PAUSE_S = 150e-6
BAUD_OPCODE = 0xFC18


def stamp(loop: asyncio.AbstractEventLoop) -> int:
    return round(loop.time() * PS_PER_SECOND)


def event(opcode: int) -> bytes:
    return bytes([0x04, 0x0E, 0x04, 0x01, opcode & 0xFF, opcode >> 8, 0x00])


class Peer(UartDevice):
    """Answers each H4 command 100 us after its last byte. After the reset pin rises it boots
    `boot_s` later, samples the strap pin, sends a hello event and waits `ACK_TIMEOUT_S` for a
    command. Its `snapshot` and `restore` keep the link state: the boot and ack deadlines, the
    bytes of a half-read command and the replies still due, so a stack that heard traffic can be
    restored at any stamp."""

    def __init__(self, *, boot_s: float = 200e-6, pause_after: int | None = None) -> None:
        super().__init__()
        self.boot_s = boot_s
        self.pause_after = pause_after
        self.state = "reset"
        self.strap = False
        self.count = 0
        self.acked = False
        self.partial: list[int] = []
        self.pending: list[tuple[int, int]] = []
        self.log: list[tuple[str, int]] = []
        self.rts_edges: list[tuple[bool, int]] = []
        self.boot_due = 0
        self.ack_due = 0
        self._ack = asyncio.Event()
        self._tasks: list[asyncio.Future[None]] = []

    def on_attach(self) -> None:
        self.state = "reset"
        self.count = 0
        self.acked = False
        self.partial = []
        self.pending = []
        self.log.clear()
        self._build()

    def _build(self) -> None:
        self._ack = asyncio.Event()
        if self.acked:
            self._ack.set()
        self._tasks = []
        self.board.watch_pin(RESET_PIN, self._reset_edge)

    def _reset_edge(self, level: bool, at: VirtualTime) -> None:
        if not level or self.state != "reset":
            return
        self.state = "released"
        self.strap = self.board.pin_level(STRAP_PIN)
        self.log.append(("released", at.ps))
        self.boot_due = stamp(self.loop) + round(self.boot_s * PS_PER_SECOND)
        self.loop.call_later(self.boot_s, self._boot)

    def _boot(self) -> None:
        self.state = "up"
        self.log.append(("boot", stamp(self.loop)))
        self.send(bytes([0x04, 0xFF, 0x01, int(self.strap)]))
        self.ack_due = stamp(self.loop) + round(ACK_TIMEOUT_S * PS_PER_SECOND)
        self._tasks.append(self.loop.create_task(self._expect_ack(ACK_TIMEOUT_S)))

    async def _expect_ack(self, seconds: float) -> None:
        try:
            await asyncio.wait_for(self._ack.wait(), seconds)
        except TimeoutError:
            self.log.append(("timeout", stamp(self.loop)))

    def on_rts(self, ready: bool, at: VirtualTime) -> None:
        self.rts_edges.append((ready, at.ps))

    def on_frame(self, byte: int, at: VirtualTime) -> None:
        if not self.partial and byte != 0x01:
            self.log.append((f"junk {byte:#x}", stamp(self.loop)))
            return
        self.partial.append(byte)
        if len(self.partial) < 4 or len(self.partial) < 4 + self.partial[3]:
            return
        opcode = self.partial[2] << 8 | self.partial[1]
        self.partial = []
        self.acked = True
        self._ack.set()
        due = stamp(self.loop) + round(LATENCY_S * PS_PER_SECOND)
        self.pending.append((opcode, due))
        self._tasks.append(self.loop.create_task(self._answer(opcode, LATENCY_S)))

    async def _answer(self, opcode: int, seconds: float) -> None:
        await asyncio.sleep(seconds)
        self.pending.pop(0)
        self.count += 1
        self.log.append((f"command {opcode:#06x}", stamp(self.loop)))
        self.send(event(opcode))
        if self.count == self.pause_after:
            self.drive_cts(False)
            self.loop.call_later(CTS_PAUSE_S, self.drive_cts, True)

    def snapshot(self) -> bytes:
        return json.dumps(
            {
                "state": self.state,
                "strap": self.strap,
                "count": self.count,
                "acked": self.acked,
                "partial": self.partial,
                "pending": self.pending,
                "boot_due": self.boot_due,
                "ack_due": self.ack_due,
            }
        ).encode()

    def restore(self, data: bytes) -> None:
        saved = cast("dict[str, object]", json.loads(data))
        self.state = cast(str, saved["state"])
        self.strap = cast(bool, saved["strap"])
        self.count = cast(int, saved["count"])
        self.acked = cast(bool, saved["acked"])
        self.partial = cast("list[int]", saved["partial"])
        self.pending = [(opcode, due) for opcode, due in cast("list[list[int]]", saved["pending"])]
        self.boot_due = cast(int, saved["boot_due"])
        self.ack_due = cast(int, saved["ack_due"])
        self.log = []
        self._build()
        now = stamp(self.loop)
        if self.state == "released":
            self.loop.call_later((self.boot_due - now) / PS_PER_SECOND, self._boot)
        elif self.state == "up" and not self.acked:
            remaining = (self.ack_due - now) / PS_PER_SECOND
            self._tasks.append(self.loop.create_task(self._expect_ack(remaining)))
        for opcode, due in self.pending:
            remaining = (due - now) / PS_PER_SECOND
            self._tasks.append(self.loop.create_task(self._answer(opcode, remaining)))
