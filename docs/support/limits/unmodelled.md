# Peripheral instances

## Peripherals without a model

The [part pages](../generated/parts.md) list the instances with no model.

- A firmware access to a register of such a peripheral logs one `model_warning` per register.
  Under `strict` it raises `ModelError`.
- A host access over the bus, through `m.mem` or a register's `read()` and `write()`, raises
  `ModelError` and logs nothing, with or without `strict`.
- The `value` getter, the field getters and `poke()` reach the register storage without a model.
- An offset no register covers inside a modelled peripheral counts as unmodelled data too.
- A firmware or DMA access to an offset no register covers, in a peripheral with or without a
  model and held in RCC reset or not, reads 0, drops a write and logs one `model_warning` per
  offset. Under `strict` it raises `ModelError`. A native model's or plugin's debugger access
  through the services there under `strict` reads 0, drops a write and logs nothing.
- The debug blocks TPIU, ETM, CTI and the ROM tables have no model. A firmware or DMA access
  reads 0 and drops writes, with one `model_warning` per block. The ROM tables read 0, so a ROM
  table walk ends at its first entry.

## Instances a package drops

A CubeMX part file lists the IPs its package brings out. An SVD instance that the part's file
leaves out takes the IP name and version of the die reference, the part of the same die and SVD
with the most bonded GPIO pins, then the largest flash, then the first RefName. The instance then
gets that IP's patches and built-in model, as on the reference. UART5 and SAI2 on the STM32U575
and U585 C packages and SPI4, UART8 and USART10 on the 64-pin STM32H5 parts are such instances.

- This rule is inferred from the die. Its supporting reading is on a STM32U575ZITxQ, an LQFP144
  that bonds no port I pin, where `GPIOI_MODER` reads 0x0000FFFF, the reset value of the 8 pins
  of port I: an unbonded block keeps its registers.
- An instance that no part of the die and SVD lists keeps the SVD group as its IP and an empty
  version, and has no model.
