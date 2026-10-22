# Faults and errors

A processor fault either stops the run or enters the firmware's handler. `faults=` on the
machine description chooses:

```python
--8<-- "guide/test_faults.py:descriptions"
```

## A fault stops the run

Under the default `faults="stop"`, a fault ends the run with a FAULT stop at the faulting
instruction. Here the test makes the core jump to address 0, as a call through a null function
pointer does:

```python
--8<-- "guide/test_faults.py:stop"
```

`stop.fault` is a `popcorn.Fault`:

- `kind`, a `popcorn.FaultKind`, such as BUS or USAGE.
- `address`, the data address an access faulted at, or the instruction address of a fetch.
- `cfsr` and `hfsr`, the raw fault status registers. Here CFSR has IBUSERR set, bit 8.

Nothing is mapped at address 0 on the STM32U5, as on silicon. A load or store through a null
pointer stops the same way, with PRECISERR and BFARVALID set and BFAR 0.

A failing test's report decodes the fault registers and shows the call stack.

## The firmware takes the fault

Under `faults="handler"`, the core enters the firmware's HardFault, MemManage, BusFault,
UsageFault or SecureFault handler, as silicon does, and the run goes on:

```python
--8<-- "guide/test_faults.py:handler"
```

The BusFault here escalates to HardFault, because the firmware did not enable BusFault in
SHCSR: IPSR reads 3 and HFSR has FORCED set. Some faults stop the run under both policies, such as a lockup.
[Limits](../support/limits.md#core-faults) lists them.

## Host accesses

A host read or write through `m.mem` that the bus refuses raises `popcorn.BusAccessError`, a
`popcorn.EmulationError`. It does not fault the core.

```python
--8<-- "guide/test_faults.py:host"
```

## Unmodelled data

A host access over the bus to data no model serves raises `ModelError` and logs nothing,
whatever `strict` says, with or without TrustZone. The register storage stays reachable:

```python
--8<-- "guide/test_faults.py:unmodelled"
```

The WWDG of the STM32U575 has no model, and neither has the CTI at 0xE0042000. `read()` raises,
`poke()` stores without any model involved, and the `value` getter reads the storage.

Host accesses over the bus are `m.mem` with its `secure` and `nonsecure` views, `m.ref.mem`,
and a register's `read()`, `write()` and `value` setter. Unmodelled data is:

- every register of a peripheral without a model
- an offset no register covers inside a modelled peripheral
- a register a model keeps as storage only, such as every SYSCFG register but `SECCFGR`
- the STM32U5 and STM32H5 debug blocks without a model: TPIU 0xE0040000, ETM 0xE0041000, CTI
  0xE0042000, the MCU ROM table 0xE00FE000 and the processor ROM table 0xE00FF000

The `value` getter, the field getters and `poke()` go to the register storage, without the
model. Firmware and DMA accesses follow other rules:

- A firmware access to a register without a model logs a model warning once per register. Under
  `strict` it raises `ModelError`.
- A firmware write to a SYSCFG storage register lands and logs one model warning per register.
  Under `strict` it raises `ModelError` before the store.
- A firmware or DMA access to a debug block without a model reads 0 and drops writes, with one
  model warning per block. Under `strict` it raises `ModelError` naming the block. The ROM tables
  read 0, not their entries, so a ROM table walk ends at its first entry.

## Errors

popcorn raises these exceptions, all exported from `popcorn`:

| Exception | Raised when |
|---|---|
| `ConfigError` | A description, option or argument is invalid. |
| `MissingInput` | An image, a description source or an ST install is missing. |
| `ImageError` | An image cannot be read or does not fit the memory map. |
| `SymbolError` | A symbol or variable name is not in the loaded images. |
| `EmulationError` | A run, call or host access cannot complete. |
| `FaultError` | A processor fault stopped a step that expected to finish, such as `boot_machine`. |
| `BudgetExceeded` | An instruction budget ran out where a stop was expected, such as in `boot_machine`. |
| `ModelError` | A model failed, or an unmodelled access ran under `strict`. |
| `UnmodelledConfig` | The firmware configured a feature a model does not cover. |
| `BusAccessError` | The bus refused a host access through `m.mem`. |
| `MachineBusy` | A run, step, call or wait started while another is active. |
