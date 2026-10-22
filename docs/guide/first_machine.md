# First machine

This page builds a machine from the usart example firmware, boots it, talks to it over USART1
and reads its state. The firmware prints a banner, then echoes each line it receives as
`echo: <line>`.

## Describe the machine

```python
--8<-- "guide/test_first_machine.py:describe"
```

`ELF` is the path of the firmware. This guide uses the prebuilt ELFs of the repository's examples,
`examples/stm32u575/<name>/prebuilt/<name>.elf` in a checkout, so a pip install does not
contain them. Point `popcorn.Elf` at your own firmware ELF to follow along. The description
names the part and the ELF to load. `Boot` resets the core and runs to `main`
within 1,000,000 instructions, so the C runtime start-up is done when the test begins.

## Build and boot it

```python
--8<-- "guide/test_first_machine.py:boot"
```

`machine()` builds the machine powered off. `boot_machine()` resets it and runs it to
`boot.until`. It returns the ADDRESS stop at `main` and raises when the firmware faults or runs
out of budget first.

These calls are coroutines. A script runs them under `asyncio.run()`:

```python
--8<-- "guide/test_first_machine.py:script"
```

Under pytest, the popcorn plugin runs async tests on its own loop, see
[Testing with pytest](testing.md).

## Talk to the firmware

```python
--8<-- "guide/test_first_machine.py:uart"
```

`m.periph.USART1.port` is the far end of the USART line. `read(n, for_=...)` runs the machine
until the firmware has sent `n` bytes, or until `for_` of virtual time has passed, and returns
what arrived. Without `for_`, it returns fewer bytes when the core goes idle. `write()` queues
bytes on the firmware's RX line, one frame time apart at the baud rate the firmware set. It
returns an awaitable, so the test awaits it.

## Read its state

```python
--8<-- "guide/test_first_machine.py:state"
```

Once a run returns, the machine is parked and its state reads directly:

- `m.var(name)` finds a firmware variable by name and types it from DWARF. Index an array, read
  `.value`, or assign `.value` to write it.
- `m.periph.<instance>.<register>.value` reads the register storage without side effects.
- `m.reg` holds the core registers.

The [Python reference](../reference/python/index.md) lists every accessor.
