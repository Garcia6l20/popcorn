"""A whole H4-like peer on a UART device port: framed commands answered 100 us later, a reset and
a strap pin, a timeout, a baud change from 115200 to 921600, DMA both ways with RTS and CTS. Two
runs give the same tap, byte for byte with stamps."""

from __future__ import annotations

import asyncio
import itertools
from dataclasses import dataclass
from pathlib import Path

import popcorn
import pytest
from popcorn import SnapshotRefused, System, UartLink
from popcorn.board import UartDevice
from popcorn.units import ms, us

from .linked import member
from .peer import (
    ACK_TIMEOUT_S,
    LATENCY_S,
    PS_PER_SECOND,
    Peer,
    event,
)
from .test_restore_rule import asleep, run

BATCH = 16
PEER_LL = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "peer_ll.elf"
LATENCY_PS = round(LATENCY_S * PS_PER_SECOND)
HELLO = bytes([0x04, 0xFF, 0x01, 0x01])


@pytest.fixture(scope="module")
def u575() -> popcorn.PartDescription:
    from popcorn.stm32 import mcu

    return mcu("STM32U575QGIx")


@dataclass(frozen=True)
class Outcome:
    tap: list[popcorn.TapFrame]
    log: list[tuple[str, int]]
    hello: bytes
    first: bytes
    baud: bytes
    batch: bytes
    count: int
    rts: list[tuple[bool, int]]
    end_ps: int


def read(m: popcorn.Machine, name: str, size: int) -> bytes:
    return bytes(m.mem.read(m.sym(name), size))


def outcome(m: popcorn.Machine, peer: Peer) -> Outcome:
    return Outcome(
        peer.tap_frames(),
        list(peer.log),
        read(m, "peer_hello", 4),
        read(m, "peer_first", 7),
        read(m, "peer_baud", 7),
        read(m, "peer_in", 7 * BATCH),
        peer.count,
        list(peer.rts_edges),
        m.time.ps,
    )


def lone(u575: popcorn.PartDescription) -> Outcome:
    peer = Peer(pause_after=8)
    peer.tapping = True
    m = popcorn.Machine(u575, image=popcorn.Image.elf(PEER_LL), devices={"USART1": [peer]})
    m.reset()
    asyncio.run(m.run(until="main", budget=1_000_000))
    m.reg.pc = m.sym("peer_host")
    asyncio.run(m.run(for_=ms(100)))
    return outcome(m, peer)


def on_a_member(u575: popcorn.PartDescription) -> Outcome:
    peer = Peer(pause_after=8)
    peer.tapping = True
    a = popcorn.Machine(
        u575,
        image=popcorn.Image.elf(PEER_LL),
        name="a",
        devices={"USART1": [peer]},
        slice_instructions=1_000_000,
    )
    a.reset()
    asyncio.run(a.run(until="main", budget=1_000_000))
    a.reg.pc = a.sym("peer_host")
    b = member(u575, "idle", "b")
    system = System(
        {"a": a, "b": b}, [UartLink("a.USART2", "b.USART2", latency=us(20), baud=115200)]
    )
    with system:
        asyncio.run(system.run(for_=ms(100)))
    return outcome(a, peer)


def frames(result: Outcome, towards: str) -> list[popcorn.TapFrame]:
    return [frame for frame in result.tap if frame.towards == towards]


def stream(result: Outcome, towards: str) -> bytes:
    return bytes(frame.data for frame in frames(result, towards))


def expected_replies() -> bytes:
    return (
        HELLO
        + event(0x0C03)
        + event(0xFC18)
        + b"".join(event(0x0C20 + index) for index in range(BATCH))
    )


def test_two_runs_give_the_same_whole_port_tap(u575: popcorn.PartDescription) -> None:
    first = lone(u575)
    second = lone(u575)
    assert first == second
    assert stream(first, "USART1") == expected_replies()
    assert len(first.tap) == 4 + 4 + 7 + 4 + 7 + 4 * BATCH + 7 * BATCH
    assert first.hello == HELLO
    assert first.first == event(0x0C03)
    assert first.baud == event(0xFC18)
    assert first.batch == b"".join(event(0x0C20 + index) for index in range(BATCH))
    assert first.count == 2 + BATCH
    assert [name for name, _ in first.log if name.startswith("command")] == [
        "command 0x0c03",
        "command 0xfc18",
        *(f"command {0x0C20 + index:#06x}" for index in range(BATCH)),
    ]


def test_no_byte_is_lost_with_dma_both_ways_and_flow_control_at_921600(
    u575: popcorn.PartDescription,
) -> None:
    result = lone(u575)
    sent = frames(result, "device")
    assert len(sent) == 8 + 4 * BATCH
    batch = sent[8:]
    gaps = [after.start.ps - before.end.ps for before, after in itertools.pairwise(batch)]
    assert max(gaps) >= us(100).ps
    assert {frame.end.ps - frame.start.ps for frame in batch} == {10_875_000}


def test_no_reply_starts_while_the_firmware_holds_rts_low(u575: popcorn.PartDescription) -> None:
    result = lone(u575)
    lows = [
        (before[1], after[1])
        for before, after in itertools.pairwise(result.rts)
        if not before[0] and after[0]
    ]
    assert len(lows) > 100
    starts = [frame.start.ps for frame in frames(result, "USART1")]
    assert all(not low < start < high for low, high in lows for start in starts)
    assert any(start == high for _, high in lows for start in starts)


def test_a_reply_lands_at_the_configured_latency_exactly(u575: popcorn.PartDescription) -> None:
    result = lone(u575)
    sent = frames(result, "device")
    answered = frames(result, "USART1")
    assert answered[4].start.ps == sent[3].end.ps + LATENCY_PS
    assert answered[11].start.ps == sent[7].end.ps + LATENCY_PS
    assert answered[18].start.ps == sent[11].end.ps + LATENCY_PS
    for command in range(BATCH):
        assert answered[18 + 7 * command].start.ps >= sent[11 + 4 * command].end.ps + LATENCY_PS


def test_the_same_peer_on_a_system_member_gives_the_same_bytes(
    u575: popcorn.PartDescription,
) -> None:
    alone = lone(u575)
    member_run = on_a_member(u575)
    assert stream(member_run, "device") == stream(alone, "device")
    assert stream(member_run, "USART1") == stream(alone, "USART1")
    assert [(f.start.ps, f.end.ps) for f in member_run.tap] == [
        (f.start.ps, f.end.ps) for f in alone.tap
    ]
    assert on_a_member(u575) == member_run


def poked(u575: popcorn.PartDescription, device: Peer, *, strap: bool = True) -> popcorn.Machine:
    m = asleep(u575, {"USART1": [device]})
    m.periph.GPIOA.MODER.MODE1 = 1
    m.periph.GPIOA.MODER.MODE2 = 1
    if strap:
        m.periph.GPIOA.BSRR.BS2 = 1
    return m


def command(m: popcorn.Machine, opcode: int) -> None:
    for byte in (0x01, opcode & 0xFF, opcode >> 8, 0x00):
        m.periph.USART1.TDR.value = byte
        run(m, us(100))


def release(m: popcorn.Machine, device: Peer) -> None:
    m.periph.GPIOA.BSRR.BS1 = 1


def test_a_timeout_fires_at_its_exact_stamp_and_sends_nothing(
    u575: popcorn.PartDescription,
) -> None:
    device = Peer(boot_s=0.451)
    device.tapping = True
    m = poked(u575, device)
    run(m, ms(244))
    release(m, device)
    run(m, ms(900))
    stamps = dict(device.log)
    assert stamps["boot"] - stamps["released"] == round(0.451 * PS_PER_SECOND)
    assert stamps["timeout"] - stamps["boot"] == round(ACK_TIMEOUT_S * PS_PER_SECOND)
    assert stream_of(device, "USART1") == HELLO
    assert device.tap_frames("device") == []


def stream_of(device: UartDevice, towards: str) -> bytes:
    return bytes(frame.data for frame in device.tap_frames(towards))


class DefaultSnapshot(Peer):
    snapshot = UartDevice.snapshot
    restore = UartDevice.restore


def trace(device: Peer) -> tuple[list[tuple[str, int]], list[tuple[str, int, int, int]]]:
    log = [(name, at) for name, at in device.log if name != "released"]
    taken = [(f.towards, f.data, f.start.ps, f.end.ps) for f in device.tap_frames()]
    return log, [(t, d, s, e) for t, d, s, e in taken]


def test_a_default_stack_refuses_past_the_reset_edge(u575: popcorn.PartDescription) -> None:
    plain = DefaultSnapshot(boot_s=0.451)
    m = poked(u575, plain)
    run(m, ms(244))
    release(m, plain)
    run(m, ms(141))
    with pytest.raises(SnapshotRefused, match="override snapshot"):
        m.snapshot()


def test_a_default_stack_restored_ahead_of_the_reset_edge_boots_at_the_same_stamp(
    u575: popcorn.PartDescription,
) -> None:
    plain = DefaultSnapshot(boot_s=0.451)
    plain.tapping = True
    m = poked(u575, plain)
    run(m, ms(100))
    saved = m.snapshot()

    def to_the_first_answer() -> tuple[object, ...]:
        run(m, ms(144))
        release(m, plain)
        run(m, ms(700))
        command(m, 0x0C03)
        run(m, ms(1))
        return trace(plain)

    went_on = to_the_first_answer()
    m.restore(saved)
    plain.tap_frames()
    assert to_the_first_answer() == went_on


def test_a_snapshot_out_of_reset_with_no_byte_seen_restores_and_answers(
    u575: popcorn.PartDescription, tmp_path: Path
) -> None:
    device = Peer(boot_s=0.451)
    device.tapping = True
    m = poked(u575, device)
    run(m, ms(244))
    release(m, device)
    run(m, ms(141))
    device.tap_frames()
    saved = m.snapshot()
    path = tmp_path / "out_of_reset.snap"
    saved.save(path)
    assert m.time.ps == ms(385).ps

    def after() -> tuple[list[tuple[str, int]], list[tuple[str, int, int, int]]]:
        run(m, ms(700))
        command(m, 0x0C03)
        run(m, ms(1))
        return trace(device)

    went_on = after()
    assert dict(went_on[0])["boot"] == ms(695).ps
    assert [frame[1] for frame in went_on[1] if frame[0] == "USART1"] == [*HELLO, *event(0x0C03)]

    m.restore(saved)
    device.tap_frames()
    assert after() == went_on

    fresh = Peer(boot_s=0.451)
    fresh.tapping = True
    other = poked(u575, fresh)
    other.restore(other.load_snapshot(path))
    run(other, ms(700))
    command(other, 0x0C03)
    run(other, ms(1))
    assert trace(fresh) == went_on


def booted(u575: popcorn.PartDescription) -> tuple[popcorn.Machine, Peer]:
    device = Peer()
    device.tapping = True
    m = poked(u575, device)
    run(m, ms(1))
    release(m, device)
    run(m, ms(1))
    return m, device


def send_bytes(m: popcorn.Machine, data: bytes) -> None:
    for byte in data:
        m.periph.USART1.TDR.value = byte
        run(m, us(100))


@pytest.mark.parametrize("point", ["mid_command", "reply_window", "up_and_not_acked"])
def test_a_snapshot_inside_a_command_a_reply_window_or_the_ack_wait_restores_as_it_went_on(
    u575: popcorn.PartDescription, tmp_path: Path, point: str
) -> None:
    m, device = booted(u575)
    command_bytes = bytes([0x01, 0x03, 0x0C, 0x00])
    if point == "mid_command":
        send_bytes(m, command_bytes[:2])
    elif point == "reply_window":
        send_bytes(m, command_bytes)
    else:
        run(m, ms(60))
    device.log.clear()
    device.tap_frames()
    saved = m.snapshot()
    path = tmp_path / "point.snap"
    saved.save(path)

    def rest(machine: popcorn.Machine) -> None:
        if point == "mid_command":
            send_bytes(machine, command_bytes[2:])
        run(machine, ms(300))

    rest(m)
    went_on = trace(device)
    names = [name for name, _ in went_on[0]]
    assert names == (["timeout"] if point == "up_and_not_acked" else ["command 0x0c03"])
    answered = [frame[1] for frame in went_on[1] if frame[0] == "USART1"]
    assert answered == ([] if point == "up_and_not_acked" else list(event(0x0C03)))

    m.restore(saved)
    device.tap_frames()
    rest(m)
    assert trace(device) == went_on

    fresh = Peer()
    fresh.tapping = True
    other = poked(u575, fresh)
    other.restore(other.load_snapshot(path))
    rest(other)
    assert trace(fresh) == went_on


def digest(result: Outcome) -> str:
    import hashlib

    return hashlib.sha256(repr(result).encode()).hexdigest()


def test_the_lone_run_gives_the_same_digest_in_another_process(
    u575: popcorn.PartDescription,
) -> None:
    import os
    import subprocess
    import sys

    root = Path(__file__).parents[3]
    code = (
        "from popcorn.stm32 import mcu\n"
        "from tests.python.api.test_uart_peer import digest, lone\n"
        "print(digest(lone(mcu('STM32U575QGIx'))))\n"
    )
    env = {**os.environ, "PYTHONHASHSEED": "7", "PYTHONPATH": os.pathsep.join(sys.path)}
    done = subprocess.run(
        [sys.executable, "-c", code], cwd=root, env=env, capture_output=True, text=True, timeout=60
    )
    assert done.returncode == 0, done.stderr
    assert done.stdout.strip() == digest(lone(u575))
