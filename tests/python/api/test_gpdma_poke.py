"""A GPDMA1 channel whose CxTR2 is poked follows the new request at the next request."""

from __future__ import annotations

import asyncio
from pathlib import Path

import popcorn
from popcorn.units import us

RCC_LL = Path(__file__).parents[2] / "firmware" / "periph" / "prebuilt" / "rcc_ll.elf"
USART1_TX = 25
USART2_TX = 27
SOURCE = 0x2000_0100
TARGET = 0x2000_0200
DATA = bytes(range(1, 9))


def booted() -> popcorn.Machine:
    m = popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(RCC_LL)]).machine()
    m.reset()
    m.reg.pc = m.sym("idle")
    return m


def run(m: popcorn.Machine) -> None:
    asyncio.run(m.run(for_=us(50)))


def test_a_poked_reqsel_routes_the_next_request_to_the_channel() -> None:
    m = booted()
    m.periph.RCC.RCC_AHB1ENR.GPDMA1EN = 1
    m.periph.RCC.RCC_APB1ENR1.USART2EN = 1
    m.mem.write(SOURCE, DATA)
    m.mem.write(TARGET, bytes(len(DATA)))
    uart = m.periph.USART2
    uart.CR3.DMAT = 1
    uart.CR1_disabled.UE = 1
    uart.CR1_enabled.TE = 1
    dma = m.periph.GPDMA1
    dma.GPDMA_C0TR1.SINC = 1
    dma.GPDMA_C0TR1.DINC = 1
    dma.GPDMA_C0TR2.REQSEL = USART1_TX
    dma.GPDMA_C0BR1.BNDT = len(DATA)
    dma.GPDMA_C0SAR.value = SOURCE
    dma.GPDMA_C0DAR.value = TARGET
    dma.GPDMA_C0CR.EN = 1
    uart.CR3.DMAT = 1
    run(m)
    assert m.mem.read(TARGET, len(DATA)) == bytes(len(DATA))
    dma.GPDMA_C0TR2.poke(REQSEL=USART2_TX)
    uart.CR3.DMAT = 1
    run(m)
    assert m.mem.read(TARGET, len(DATA)) == DATA
