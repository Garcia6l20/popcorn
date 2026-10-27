"""UART9 TX fed by GPDMA1 channel 0 from HAL: a normal block, then a two-node linked list.

`HAL_UART_Transmit_DMA` sends a 256-byte block with the channel in normal mode. The firmware
then re-initializes the channel in linked-list mode with two nodes, 64 bytes of `head` then 32
of `tail`, and sends again. Each transfer ends in `HAL_UART_TxCpltCallback`. The CPU waits in
WFI while the DMA feeds the USART.

UART9 runs from PCLK1 at the 32 MHz reset clock: a frame lasts 10 * BRR / PCLK1. The frame check
allows 1 ns.

The whole run executes about 1050 translated blocks, the WFI wait sleeping between DMA and
UART interrupts. Busy-polling the UART for the 3.85 ms the 352 frames take would run tens of
thousands of blocks at 32 MHz, so the block count bound shows the CPU asleep.

The UART port holds everything the firmware sent since the boot point the test starts from,
so the tests read it after the run.
"""

import itertools
from pathlib import Path

import pytest
from popcorn import Machine, StopReason
from popcorn.trace import Block, TraceKind, read
from popcorn.units import ms, ns

pytestmark = pytest.mark.popcorn(budget=10_000_000)

BLOCK = bytes(range(256))
HEAD = bytes(ord("A") + index % 26 for index in range(64))
TAIL = bytes(ord("a") + index % 26 for index in range(32))
PCLK1_HZ = 32_000_000
MAX_BLOCKS = 2_000
UART9_TX_REQUEST = 38


async def run_transfers(machine: Machine) -> None:
    stop = await machine.run(until="all_transfers_done")
    assert stop.reason is StopReason.ADDRESS, stop


async def test_both_transfers_arrive_in_order(gpdma: Machine) -> None:
    await run_transfers(gpdma)
    assert gpdma.var("transfers_done").value == 2

    frames = await gpdma.periph.UART9.port.read_frames(len(BLOCK + HEAD + TAIL), for_=ms(1))
    assert bytes(frame.data for frame in frames) == BLOCK + HEAD + TAIL


async def test_the_dma_keeps_the_usart_busy_back_to_back(gpdma: Machine) -> None:
    await run_transfers(gpdma)
    frames = await gpdma.periph.UART9.port.read_frames(len(BLOCK + HEAD + TAIL), for_=ms(1))

    frame_ps = 10 * gpdma.periph.UART9.BRR.value * 10**12 // PCLK1_HZ
    first, second = frames[: len(BLOCK)], frames[len(BLOCK) :]
    for transfer in (first, second):
        for earlier, later in itertools.pairwise(transfer):
            assert abs((later.at - earlier.at).ps - frame_ps) < ns(1).ps


async def test_the_cpu_sleeps_while_the_dma_moves_the_data(gpdma: Machine, tmp_path: Path) -> None:
    with gpdma.trace([TraceKind.BLOCK], tmp_path / "transfers.pctr"):
        await run_transfers(gpdma)
    blocks = sum(1 for record in read(tmp_path / "transfers.pctr") if isinstance(record, Block))
    assert blocks < MAX_BLOCKS


async def test_channel_0_serves_the_uart9_tx_request(gpdma: Machine) -> None:
    await run_transfers(gpdma)
    channel = gpdma.periph.GPDMA1
    assert channel.C0TR2.REQSEL == UART9_TX_REQUEST
    assert channel.C0CR.EN == 0
