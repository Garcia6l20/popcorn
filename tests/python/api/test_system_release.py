"""A `System` member released from its goal's stop runs its core in the next system run, a
halted one and one a system run stopped at a member stop do not."""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Awaitable, Callable, Iterator
from pathlib import Path

import popcorn
import pytest
from popcorn import StopReason, System, SystemStop, UartLink
from popcorn.board import UsbHostOptions
from popcorn.units import ms, s, us

from .linked import PING_BYTES, member, received

ROOT = Path(__file__).parents[3]
USB_CDC = ROOT / "examples" / "stm32u575" / "usb_cdc" / "prebuilt" / "usb_cdc.elf"
OTG_FS_IRQ = 73
BAD_PC = 0xEFFF_FFF0
PINGED = [0x40 + index for index in range(PING_BYTES)]


@pytest.fixture
def ping(u575: popcorn.PartDescription) -> Iterator[System]:
    a = member(u575, "link_ping", "a")
    b = member(u575, "link_echo", "b")
    link = UartLink("a.USART1", "b.USART1", latency=us(20), baud=460_800)
    system = System({"a": a, "b": b}, [link])
    yield system
    system.close()


def executed(system: System, name: str) -> int:
    index = list(system.members).index(name)
    return system._handle.executed_of(index)  # pyright: ignore[reportPrivateUsage]


async def holder_ends(system: System) -> SystemStop:
    await asyncio.create_task(system.members["b"].run(for_=us(100)))
    await asyncio.sleep(0)
    assert not system._undecided  # pyright: ignore[reportPrivateUsage]
    return await system.run(for_=ms(3))


async def run_releases(system: System) -> SystemStop:
    await system.members["b"].run(for_=us(100))
    return await system.run(for_=ms(3))


async def wait_for_releases(system: System) -> SystemStop:
    await system.members["b"].run(for_=us(100))
    return await system.wait_for(lambda _: False, every=ms(1), for_=ms(3))


async def peer_await_releases(system: System) -> SystemStop:
    await system.members["b"].run(for_=us(100))
    await system.members["a"].run(for_=us(1))
    return await system.run(for_=ms(3))


async def snapshot_releases(system: System) -> SystemStop:
    await system.members["b"].run(for_=us(100))
    system.snapshot()
    return await system.run(for_=ms(3))


@pytest.mark.parametrize(
    "released",
    [holder_ends, run_releases, wait_for_releases, peer_await_releases, snapshot_releases],
)
def test_a_member_released_from_its_goal_s_stop_runs_its_core_in_the_next_system_run(
    ping: System, released: Callable[[System], Awaitable[SystemStop]]
) -> None:
    async def body() -> SystemStop:
        return await released(ping)

    stop = asyncio.run(body())
    assert (stop.reason, stop.member) == (StopReason.USER, "a")
    assert received(ping.members["a"]) == PINGED


def test_a_member_stop_of_its_own_running_leaves_it_to_run_in_the_next_system_run(
    ping: System,
) -> None:
    b = ping.members["b"]
    b.reg.pc = BAD_PC

    async def body() -> SystemStop:
        with pytest.raises(popcorn.EmulationError, match="FAULT"):
            async with b.running():
                await b.elapse(ms(1))
        return await ping.run(for_=ms(3))

    stop = asyncio.run(body())
    assert (stop.reason, stop.member) == (StopReason.FAULT, "b")


def test_a_member_stop_of_the_system_s_free_run_leaves_the_member_halted(ping: System) -> None:
    async def body() -> tuple[int, SystemStop]:
        ping.start()
        opens = ping._opens  # pyright: ignore[reportPrivateUsage]
        while not (opens.get(0) is not None and opens[0].done()):
            await asyncio.sleep(0)
        with pytest.raises(popcorn.EmulationError, match="USER"):
            ping.halt()
        parked = executed(ping, "a")
        return parked, await ping.run(for_=ms(3))

    parked, stop = asyncio.run(body())
    assert stop.reason is StopReason.TIME
    assert executed(ping, "a") == parked


def test_a_halted_member_stays_halted_in_system_runs_until_its_next_goal(ping: System) -> None:
    a, b = ping.members["a"], ping.members["b"]
    asyncio.run(b.run(for_=us(100)))
    b.halt()
    parked = executed(ping, "b"), b.pc
    stop = asyncio.run(ping.run(for_=ms(3)))
    assert stop.reason is StopReason.TIME
    assert (executed(ping, "b"), b.pc) == parked
    assert received(a) == []
    asyncio.run(b.run(for_=us(10)))
    stop = asyncio.run(ping.run(for_=ms(3)))
    assert stop.reason is StopReason.TIME
    assert executed(ping, "b") > parked[0]


def test_a_halt_while_a_peer_s_goal_runs_takes_effect_at_the_next_pause(ping: System) -> None:
    a, b = ping.members["a"], ping.members["b"]

    async def body() -> SystemStop:
        await asyncio.create_task(b.run(for_=us(100)))
        await asyncio.sleep(0)
        peer = asyncio.create_task(a.run(for_=us(100)))
        while ping._flight is None:  # pyright: ignore[reportPrivateUsage]
            await asyncio.sleep(0)
        b.halt()
        await peer
        parked = executed(ping, "b")
        stop = await ping.run(for_=ms(3))
        assert executed(ping, "b") == parked
        return stop

    stop = asyncio.run(body())
    assert stop.reason is StopReason.TIME
    assert received(a) == []


def test_a_halt_inside_a_system_run_is_refused(ping: System) -> None:
    b = ping.members["b"]
    refused: list[BaseException] = []

    def asked(_: System) -> bool:
        try:
            b.halt()
        except popcorn.MachineBusy as error:
            refused.append(error)
        return True

    asyncio.run(ping.wait_for(asked, every=us(100)))
    assert len(refused) == 1


@pytest.mark.parametrize("stopper", ["breakpoint", "fault"])
def test_a_member_stop_a_system_run_found_leaves_the_member_halted(
    ping: System, stopper: str
) -> None:
    a = ping.members["a"]
    if stopper == "fault":
        a.reg.pc = BAD_PC
    first = asyncio.run(ping.run(for_=ms(3)))
    reason = StopReason.FAULT if stopper == "fault" else StopReason.USER
    assert (first.reason, first.member) == (reason, "a")
    parked = executed(ping, "a"), a.pc
    after = asyncio.run(ping.run(for_=ms(3)))
    assert after.reason is StopReason.TIME
    assert (executed(ping, "a"), a.pc) == parked


def test_a_snapshot_keeps_whether_a_released_member_is_halted(ping: System) -> None:
    a, b = ping.members["a"], ping.members["b"]

    async def body() -> popcorn.SystemSnapshot:
        await b.run(for_=us(100))
        return ping.snapshot()

    running = asyncio.run(body())
    b.halt()
    halted = ping.snapshot()

    ping.restore(running)
    stop = asyncio.run(ping.run(for_=ms(3)))
    assert (stop.reason, stop.member) == (StopReason.USER, "a")
    assert received(a) == PINGED

    ping.restore(halted)
    parked = executed(ping, "b")
    stop = asyncio.run(ping.run(for_=ms(3)))
    assert stop.reason is StopReason.TIME
    assert executed(ping, "b") == parked
    assert received(a) == []


def usb_member(u575: popcorn.PartDescription) -> popcorn.Machine:
    m = popcorn.Machine(
        u575, image=popcorn.Image.elf(USB_CDC), name="dev", usb={"OTG_FS": UsbHostOptions()}
    )
    m.reset()

    async def boot() -> None:
        await m.periph.OTG_FS.host.cdc().wait_ready(for_=s(2))
        await m.run(for_=ms(10))

    asyncio.run(boot())
    assert m.var("cdc_connected").value == 1
    return m


def test_a_member_takes_a_usb_control_request_s_interrupt_in_a_system_run(
    u575: popcorn.PartDescription,
) -> None:
    dev = usb_member(u575)
    peer = member(u575, "link_echo", "peer")
    link = UartLink("dev.USART1", "peer.USART1", latency=us(20), baud=115_200)
    with System({"dev": dev, "peer": peer}, [link]) as system:
        cdc = dev.periph.OTG_FS.host.cdc()

        async def request() -> bool:
            await cdc.set_control_line_state(dtr=False)
            return dev.nvic.pending(OTG_FS_IRQ)

        assert asyncio.run(request())
        assert dev.var("cdc_connected").value == 1
        asyncio.run(system.run(for_=ms(100)))
        assert cdc.dtr is False
        assert dev.var("cdc_connected").value == 0


def test_a_goal_cancelled_after_its_stop_leaves_its_member_to_run_in_the_next_system_run(
    ping: System,
) -> None:
    b = ping.members["b"]
    handle = ping._handle  # pyright: ignore[reportPrivateUsage]

    async def body() -> SystemStop:
        goal = asyncio.create_task(b.run(for_=us(100)))
        while 1 not in handle.undecided():
            await asyncio.sleep(0)
        goal.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await goal
        while ping._flight is not None:  # pyright: ignore[reportPrivateUsage]
            await asyncio.sleep(0)
        return await ping.run(for_=ms(3))

    stop = asyncio.run(body())
    assert (stop.reason, stop.member) == (StopReason.USER, "a")
    assert received(ping.members["a"]) == PINGED
