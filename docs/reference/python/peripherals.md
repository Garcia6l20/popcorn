# Peripherals

Registers, pins and peripheral views of a machine, and the board and device types that attach to it.

::: popcorn.Peripherals

::: popcorn.PeripheralView

::: popcorn.RegisterBank

::: popcorn.RegisterView

::: popcorn.ScalarView

::: popcorn.MemoryRef

::: popcorn.Readout

## Part descriptions

A `PartDescription` is the data of a part: its `core`, memory `regions`, `peripherals` with their
registers and fields, and `irqs`. `popcorn.stm32.mcu(part)` and `load_description` return one.

::: popcorn.PartDescription

::: popcorn.board

::: popcorn.devices

::: popcorn.model
