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

Fault classes and addresses:

- An alignment abort, such as an unaligned LDREX, is a UsageFault with UNALIGNED set in CFSR,
  not a BusFault.
- A data abort reports the data address the access faulted at, not the PC.
- An access the bus firewall refuses with a bus error is a precise BusFault with BFAR set to the
  address. A Non-secure load from a Secure peripheral, such as GTZC TZSC, is one.
- A bus error a peripheral model raises is an imprecise BusFault with no BFAR. A Python model
  raises one by returning `None` from an `on_read` handler.
- An imprecise BusFault that cannot preempt, such as one raised under FAULTMASK, pends as on
  silicon. The run stops when the BusFault, or the HardFault it escalates to, is entered, with
  the fault's kind and address and the CFSR and HFSR at entry. Clearing SHCSR.BUSFAULTPENDED
  first drops it, and the run goes on. A second such fault that pends another exception, such as
  the HardFault of a BusFault disabled in between, stops the run at once.
- With CCR.BFHFNMIGN set, a precise BusFault of a load or store made under FAULTMASK, or in the
  HardFault or NMI handler, sets PRECISERR, BFARVALID and BFAR, and the run goes on at the next
  instruction under both policies. The instruction's base register write-back applies, as on
  silicon. A load leaves the registers from the faulting access on unchanged, which the
  architecture leaves UNKNOWN, and a store writes nothing from the faulting access on. A load or
  store multiple ends at the faulting access, where silicon makes the remaining ones.

A failing test's report decodes the fault registers and shows the call stack.
A test that raised a `ModelError`, such as from a strict model, shows the error once, and each
machine with no stop shows its registers and the call stack from them. A model error is raised
at the end of the block holding the access, so the first frame of the machine that raised it can
be later than the accessing instruction, often in its caller.

## The firmware takes the fault

Under `faults="handler"`, the core enters the firmware's HardFault, MemManage, BusFault,
UsageFault or SecureFault handler, as silicon does, and the run goes on:

```python
--8<-- "guide/test_faults.py:handler"
```

The BusFault here escalates to HardFault, because the firmware did not enable BusFault in
SHCSR: IPSR reads 3 and HFSR has FORCED set. Some faults stop the run under both policies, such as a lockup.
[Limits](../support/generated/limits/core.md#core-faults) lists them.

A handler that sets FAULTMASK and returns from HardFault keeps it set on an ARMv8-M core, and
loses it on an ARMv7-M core. Any other return, NMI's aside, clears it on both.
[Limits](../support/generated/limits/core.md#faultmask-on-exception-return) has the rule.

## Host accesses

A host read or write through `m.mem` that the bus refuses raises `popcorn.BusAccessError`, a
`popcorn.EmulationError`. It does not fault the core. `denied` is False when nothing is mapped at
the address, as at address 0, and True when a security attribute refuses the access.

```python
--8<-- "guide/test_faults.py:host"
```

## Unmodelled data

A host access over the bus to data no model serves raises `ModelError` and logs nothing,
whatever `strict` says, with or without TrustZone. The register storage stays reachable:

```python
--8<-- "guide/test_faults.py:unmodelled"
```

The TSC of the STM32U575 has no model, and neither has the CTI at 0xE0042000. `read()` raises,
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
- A firmware or DMA access to an offset no register covers reads 0, drops writes and logs a model
  warning once per offset. Under `strict` it raises `ModelError`.
- A firmware write to a SYSCFG storage register lands and logs one model warning per register.
  Under `strict` it raises `ModelError` before the store.
- A firmware or DMA access to a debug block without a model reads 0 and drops writes, with one
  model warning per block. Under `strict` it raises `ModelError` naming the block. The ROM tables
  read 0, not their entries, so a ROM table walk ends at its first entry.

## Clock gating

An STM32 peripheral whose RCC bus clock enable bit is 0 does not answer the bus. Firmware that
forgets `__HAL_RCC_CRC_CLK_ENABLE()` fails as it would on a board. In popcorn it reads zeros and
loses its writes:

```python
--8<-- "guide/test_faults.py:gated"
```

- Gated initiators are the core and the DMA controllers. Host accesses through `m.mem`, register
  `read()`, `write()` and `value`, and `poke()` are served, so a test sets registers up without
  enabling the clock.
- A read of a gated peripheral gives 0 and a write is lost. No model handler runs.
- Each gated peripheral logs one model warning per machine, at its first gated access. Enabling
  the clock does not rearm it.
- Under `strict` or `strict_peripherals` the access raises `ModelError`:

```python
--8<-- "guide/test_faults.py:gated_strict"
```

- `machine.periph.<NAME>.clock_gated` tells whether the clock is gated. Writing the enable bit
  updates it at once.
- The enable gates register access only. Timers keep counting, transfers keep running and
  events stay queued while the clock is off.
- Read 0 and write lost are inferred, the reference manuals say only that access is not
  supported. The [RCC limits](../support/generated/limits/rcc.md#clock-gates) list the instances
  gated on a host block's bit.

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
| `BudgetExceeded` | An instruction budget ran out where a stop was expected, such as in `boot_machine`. `budget` is the budget given, `instructions` the count the run reached. |
| `ModelError` | A model failed, or an unmodelled access ran under `strict`. |
| `UnmodelledConfig` | The firmware configured a feature a model does not cover. |
| `BusAccessError` | The bus refused a host access through `m.mem`. |
| `MachineBusy` | A run, step, call or wait started while another is active. |
