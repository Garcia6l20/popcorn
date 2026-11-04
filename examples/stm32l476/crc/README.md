# CRC calculation unit, HAL

An STM32L476 application computes three CRCs through ST's HAL CRC driver on the programmable CRC
unit of RM0351 section 15, then keeps a byte in the 8-bit CRC_IDR, RM0351 15.4.2: CRC-32/MPEG-2
on words with `HAL_CRC_Calculate` and `HAL_CRC_Accumulate`, CRC-32/ISO-HDLC with input and
output reversal on the bytes "123456789", and CRC-16/IBM-3740 with the polynomial 0x1021 from
0xFFFF. The tests check each result against a bit-serial Python reference.

It ports `stm32u575/crc`. The HAL feeds the byte buffers four bytes per word write, then a
half-word and a byte write, so the byte cases also run the 16- and 8-bit CRC_DR writes.
`MachineDescription(strict=True)` makes any register access the CRC model does not serve fail
the run instead of being logged.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/crc
```

## Read first

- `test_crc_l476.py`: the reference and the catalogue check values, the results read with
  `machine.var("results")` at `computed`, and CRC_IDR through the peripheral view.
- `firmware/main.c`: the three `HAL_CRC_Init` configurations and the calls.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/crc/crc.elf examples/stm32l476/crc/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld` for 256 KB flash and the 96 KB SRAM1.
