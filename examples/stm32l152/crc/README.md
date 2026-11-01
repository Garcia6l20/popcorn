# CRC calculation unit, HAL

An STM32L152 application computes CRCs with ST's HAL CRC driver. The STM32L1 CRC unit has one
fixed configuration, RM0038 section 4: polynomial 0x04C11DB7 from 0xFFFFFFFF on 32-bit words,
CRC-32/MPEG-2. The firmware runs `HAL_CRC_Calculate` over one word, `HAL_CRC_Accumulate` over a
second one, and `HAL_CRC_Calculate` over an 8-word block, then keeps a byte in CRC_IDR. The tests
check each result against a bit-serial Python reference.

It ports `stm32u575/crc`, which also programs the polynomial, the size and the reversal. The L1
unit has none of these.

`MachineDescription(strict=True)` makes any register access the CRC model does not serve fail the
run instead of being logged.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l152/crc
```

## Read first

- `test_crc_l152.py`: the reference and its catalogue check value, the results read through
  `machine.var`, CRC_DR and CRC_IDR read through the peripheral view.
- `firmware/main.c`: the three CRC calls.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m3/examples/stm32l152/crc/crc.elf examples/stm32l152/crc/prebuilt/
```

The build links `../STM32L152XE_FLASH.ld`, 256 KB flash and 32 KB SRAM, so the stack lies inside
the SRAM of every part it runs on.
