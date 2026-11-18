# One-time programmable memory

The OTP area of the STM32U5, STM32H5 and STM32L4 is erased at power-on, written by the host at
any time, and programmed by firmware through the FLASH interface once per unit. The content and
the H5 bookkeeping survive every reset, a power cycle and a snapshot restore.

| line | OTP area | unit programmed by firmware |
|---|---|---|
| STM32U575, STM32U585 | `0x0BFA0000` to `0x0BFA01FF`, Secure alias `0x0FFA0000` | quad-word of 128 bits |
| STM32H562, STM32H563, STM32H573 | `0x08FFF000` to `0x08FFF7FF` | half-word of 16 bits |
| STM32L4 | `0x1FFF7000` to `0x1FFF73FF` | double word of 64 bits |

## Provision from the host

```python
--8<-- "guide/test_otp.py:provision"
```

`machine.mem.write` writes the OTP with no flag, no lock and no ECC bookkeeping. On the U5 the
Secure alias reads the same bytes. On the H5 a half-word the host wrote with data other than
`0xFFFF` counts as programmed, so a later firmware program of it is a rewrite.

## Program from firmware on the U5

```python
--8<-- "guide/test_otp.py:u5"
```

With `PG` set in `FLASH_NSCR`, four word stores at a 16-byte aligned address program a quad-word
for the quad-word program time, and a burst of eight quad-words programs 128 bytes. A quad-word
with any bit at 0 sets `PROGERR` and keeps its bytes, also for zero data. A Secure store sets the
Secure `WRPERR`, raises `FLASHIE` and writes nothing. A store to the 512 bytes past the OTP area
sets the Non-secure `WRPERR`, inferred. `SIZERR`, `PGAERR` and `PGSERR` apply as for the main
memory and no erase reaches the OTP.

The L4 programs a double word of the OTP the same way with `PG`, once, and a `FSTPG` store to it
sets `WRPERR`.

## Program and read on the H5

```python
--8<-- "guide/test_otp.py:h5"
```

A 16-bit store programs one half-word and a 32-bit store two, each store starting a program for
31 microseconds per half-word. A byte store and a store at an odd address are a bus error,
inferred for the odd address. A store with `PG` clear, with an erase selected, with a flag pending
or with the main write buffer open sets `PGSERR` and writes nothing.

Setting a bit of `FLASH_OTPBLR_PRG` and then `OPTSTRT` locks the 64-byte block it names. The bit
reaches the read-only `FLASH_OTPBLR_CUR`, never clears, and a store to the block sets `WRPERR`.

A CPU, DMA or debug port load of a half-word that was never programmed, or that was programmed
twice, sets `ECCD`, `OTP_ECC`, `ADDR_ECC` and `FLASH_ECCDR.DATA_ECC` and raises the NMI.
`ADDR_ECC` counts 16-byte units and runs from `0x700`, inferred. Setting
`SBS_ECCNMIR.ECCNMI_MASK_EN` records the error without the NMI, which is what a firmware that
tests a half-word for virgin does first. A power cut during an OTP program tears the half-word in
flight, inferred.

An 8-bit or 64-bit load of the OTP returns its data and warns once, and raises `ModelError` under
`strict_peripherals`. `FLASH_OPSR` is storage.

The rules marked inferred have no board check, see the [FLASH limits](../support/generated/limits/flash.md).
