# UART TX over GPDMA, HAL

An STM32H573 application sends on UART9 TX, PG1, with GPDMA1 channel 0 moving the bytes.
First `HAL_UART_Transmit_DMA` sends a 256-byte block with the channel in normal mode. Then
the firmware re-initializes the channel in linked-list mode. The queue has two nodes: 64 bytes
of `head`, whose address and length HAL fills in from the call, then 32 bytes of `tail`. It
sends again. Every list node carries the last-item transfer event mode, like the channel, so
the list raises one transfer-complete at its end. `HAL_UART_TxCpltCallback` counts the finished
transfers, and the CPU waits in WFI meanwhile.

It ports `stm32u575/gpdma`. The H5 request is `GPDMA1_REQUEST_UART9_TX`, number 38 of the 138
requests of RM0481 Table 142, where the U5 sends USART1 TX.

Re-initializing the DMA handle clears its parent, so the firmware links it to the UART again
with `__HAL_LINKDMA` after `HAL_DMAEx_List_LinkQ`. Without that, the transfer-complete
callback dereferences a null UART handle.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32h573/gpdma
```

## Read first

- `test_gpdma_h573.py`:
  - The UART port collects the 352 bytes, with frames back to back at the BRR rate.
  - A block trace shows the CPU mostly asleep while the DMA works.
  - The channel's request selector is the UART9 TX request.
  - The port holds what the firmware sent since the boot point the test starts from, so the
    tests read it after the run.
- `firmware/main.c`: the two channel setups, the linked-list nodes and the WFI wait.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32h573/gpdma/gpdma_uart.elf examples/stm32h573/gpdma/prebuilt/
```
