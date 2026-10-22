# Python models and devices

popcorn ships models for most STM32U5 peripherals. When a test needs different behavior, write
the peripheral or its far end in Python. There are two kinds of extension:

- A peripheral model, a `popcorn.model.Model`, replaces the behavior of a peripheral: its
  registers, flags, interrupts and timing.
- A bus device sits on a peripheral that keeps its built-in model: an `I2CTarget` on an I2C bus, an
  `SpiTarget` on an SPI bus, or the host end of a UART.

Use a Python model for a peripheral popcorn does not model or models too coarsely. Use a device for
a chip on the board. Python runs on the machine thread at every handler call, so a model that
sits in a hot loop costs time. A [native plugin](native_plugins.md) is the same extension in C++.

## Write a model

```python
--8<-- "guide/test_python_models.py:model"
```

This model replaces the RNG of the STM32U575 with a counter. The pieces:

- `@on_write("CR")` and `@on_read("DR")` claim registers by name. A register answers to its bare
  name and to its SVD name, `CR` and `RNG_CR`. A handler runs with the core paused at an
  instruction boundary. `on_write` gets the old and the new value, `on_read` returns the value, or
  `None` for a bus error.
- Every register without a handler keeps its storage and its described reset value. No Python
  runs for it.
- `self.regs.SR` reads and writes stored register values. `self.fields.SR.DRDY` is the mask of a
  field.
- `self.schedule(delay, callback)` runs `callback` after a virtual delay and returns a handle.
  `self.cancel(handle)` withdraws it.
- An attribute annotated `IrqLine` receives the peripheral's interrupt line. `raise_()` and
  `lower()` drive it.
- Annotated `int`, `bool`, `float`, `bytes`, `bytearray` and `str` attributes are the state that
  snapshots save and a reset restores. Keep all model state there. A model with other state
  overrides `snapshot()` and `restore()`.
- The constructor takes the model's own parameters and calls `super().__init__()` first. A model
  never keeps a reference to its `Machine`. Reach other models with `self.peer(name)`.

## Put it on the board

```python
--8<-- "guide/test_python_models.py:describe"
```

`Periph(model=Use(cls, **params))` puts the model in the slot of one peripheral. `Use` keeps the
parameters as data, so the description stays hashable and the pytest boot cache keys on it. A
board model wins over the global registry.

```python
--8<-- "guide/test_python_models.py:run"
```

`machine.model("RNG")` returns the model object of a parked machine.

`popcorn.model.register("ip", "version-glob")` registers a class for every peripheral of an IP in
machines built afterwards, and `unregister` removes it. Prefer the board for tests, because a
registration lasts for the whole process.

## Register names

```python
--8<-- "guide/test_python_models.py:names"
```

Registers carry their bare names, `m.periph.TIM1.CR1`. The SVD prefixed names, `TIM1_CR1`, stay
valid as aliases through `m.periph`, `m.ref.periph`, Python models and the built-in models.
`RegisterDesc.name` is the bare name and `RegisterDesc.aliases` holds the SVD name.
Each `m.description.peripherals` entry has `register(name)`, which finds a register by either name and raises `KeyError` for neither.

## Bus devices

A device is a class that gets bus events from a built-in I2C, SPI or UART model:

- `popcorn.board.I2CTarget` answers an I2C address. `on_start(read, at)` ACKs or NACKs, and
  `on_write(byte, at)` and `on_read(at)` exchange the bytes.
- `popcorn.board.SpiTarget` answers an SPI transfer behind a chip select. `on_select(selected,
  at)` sees the chip select edges and `on_exchange(out, bits, at)` each frame, full duplex.
- The host end of a UART is `m.periph.USART1.port`, used from the test as in
  [First machine](first_machine.md).

`Periph(devices=[Device(TempSensor, address=0x48, readings=[21.0, 22.5])])` attaches one to a
peripheral, and `machine.board.devices("I2C1")` returns the objects. As with models, annotated
attributes are state that snapshots save.

The examples show the three uses of a mock: scripted input, a check of what the firmware sent,
and fault injection. See `i2c_mock`, `spi_mock` and `uart_mock` in the [examples](examples.md#buses).
