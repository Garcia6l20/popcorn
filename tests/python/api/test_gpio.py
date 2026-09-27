"""GPIO pins through the GPIO model, FR-PER-7."""

from __future__ import annotations

import asyncio

import popcorn
import pytest
from popcorn.board import PinMode
from popcorn.units import us


def settle(m: popcorn.Machine) -> None:
    asyncio.run(m.run(for_=us(20)))


def test_driving_an_input_reaches_the_firmware_and_its_output(board: popcorn.Machine) -> None:
    pa0, pa5 = board.periph.GPIOA.pin(0), board.periph.GPIOA.pin(5)
    pa0.level = 1
    settle(board)
    assert board.mem.u32[board.sym("pin_seen")] == 1
    assert pa0.level and pa5.level
    pa0.level = 0
    settle(board)
    assert not pa5.level
    assert board.mem.u32[board.sym("pin_seen")] == 0


def test_modes_and_alternate_functions_read_back(board: popcorn.Machine) -> None:
    gpioa = board.periph.GPIOA
    assert (gpioa.pin(0).mode, gpioa.pin(5).mode, gpioa.pin(1).mode) == (
        PinMode.INPUT,
        PinMode.OUTPUT,
        PinMode.ANALOG,
    )
    assert gpioa.pin(5).alternate == 0


def test_a_released_pin_falls_back_to_its_pull(board: popcorn.Machine) -> None:
    pa0 = board.periph.GPIOA.pin(0)
    pa0.level = 1
    assert pa0.level
    pa0.release()
    assert not pa0.level


def test_pins_are_0_to_15_on_gpio_ports(board: popcorn.Machine) -> None:
    with pytest.raises(popcorn.ConfigError):
        board.periph.GPIOA.pin(16)
    with pytest.raises(popcorn.ModelError):
        _ = board.periph.USART1.pin(0).level


def test_a_drive_inside_running_reaches_the_firmware(board: popcorn.Machine) -> None:
    async def body() -> None:
        async with board.running():
            board.periph.GPIOA.pin(0).level = 1
            for _ in range(200):
                await asyncio.sleep(0)

    asyncio.run(body())
    board.periph.GPIOA.pin(0).release()
    assert board.mem.u32[board.sym("pin_seen")] == 1
    with pytest.raises(popcorn.MachineRunning):

        async def read() -> bool:
            async with board.running():
                return board.periph.GPIOA.pin(0).level

        asyncio.run(read())


def test_a_drive_survives_a_reset_and_a_snapshot_restores_it(board: popcorn.Machine) -> None:
    pa0 = board.periph.GPIOA.pin(0)
    pa0.level = 1
    saved = board.snapshot()
    board.reset()
    assert pa0.mode is PinMode.ANALOG and not pa0.level
    asyncio.run(board.run(until=board.sym("ready"), budget=1_000_000))
    assert pa0.mode is PinMode.INPUT and pa0.level
    pa0.release()
    assert not pa0.level
    board.restore(saved)
    assert pa0.level


def test_a_drive_from_another_thread_during_a_run_is_refused(board: popcorn.Machine) -> None:
    pin = board.periph.GPIOA.pin(0)

    async def body() -> None:
        async with board.running():
            await asyncio.to_thread(setattr, pin, "level", 1)

    with pytest.raises(RuntimeError, match="another thread"):
        asyncio.run(body())
    asyncio.run(asyncio.to_thread(setattr, pin, "level", 1))
    assert pin.level
