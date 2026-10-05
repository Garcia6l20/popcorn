# CRC calculation unit, HAL

An STM32U575 application computes three CRCs through ST's HAL CRC driver, `HAL_CRC_Init`,
`HAL_CRC_Calculate` and `HAL_CRC_Accumulate`, stores them in `results`, then stops at `computed`:

| `results` field | HAL configuration | input | CRC |
| --- | --- | --- | --- |
| `one_word` | default polynomial and init, words | 0x12345678 | 0xDF8A8A2B, CRC-32/MPEG-2 |
| `two_words` | the same, accumulated | 0x9ABCDEF0 | 0x7D24A31B |
| `iso_hdlc` | input reversed by byte, output reversed, bytes, complemented | "123456789" | 0xCBF43926, CRC-32/ISO-HDLC |
| `ibm_3740` | polynomial 0x1021, 16 bits, init 0xFFFF, bytes | "123456789" | 0x29B1, CRC-16/IBM-3740 |

The HAL writes a byte buffer to CRC_DR four bytes per word, then a half-word and a byte for the
tail, so the byte cases also run the 16- and 8-bit CRC_DR writes. `conftest.py` sets
`strict=True`, so an access the CRC model does not serve fails the run.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/crc
```

## Read first

- `test_crc.py`:
  - `reference` is a bit-serial CRC in the catalogue's parameter model, typed pure Python.
    The test checks it against the catalogue check values, then each result against it.
  - The firmware's results are read with `machine.var("results")` once the run stops at
    `computed`.
  - The first test also checks that the run logs nothing.
- `firmware/main.c`: the three `HAL_CRC_Init` configurations and the calls.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/crc/crc.elf examples/stm32u575/crc/prebuilt/
```
