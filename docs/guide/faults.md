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
