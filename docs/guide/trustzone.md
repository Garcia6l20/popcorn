# TrustZone

popcorn runs the STM32U575 with TZEN=1. It models the Cortex-M33 Secure and Non-secure states,
the SAU and the IDAU, the banked SCB, NVIC, SysTick and MPU registers, SG, BXNS, BLXNS,
FNC_RETURN and SecureFault. On the STM32 side it models the GTZC TZSC, TZIC and MPCBB, the
flash secure areas, HDP and PRIVBB, the 0x5xxx secure aliases, and the `SECCFGR` and `PRIVCFGR`
bits of RCC, PWR, GPIO, EXTI, SYSCFG, FLASH, GPDMA and LPDMA. `OptionBytes(tzen=True)` turns it
on:

```python
--8<-- "guide/test_trustzone.py:describe"
```

- The core boots Secure from `secboot`, 0x0C000000 by default on the STM32U5.
- `secwm1` and `secwm2` are the Secure flash pages of each bank, first and last included. The
  default `"all"` makes the whole bank Secure, as after TZEN goes from 0 to 1.
- `option_bytes=None` keeps the description's option registers, and TrustZone stays off.
- `name=` names an image. Two images of one name are a ConfigError.
- `loaded=False` makes a symbols-only image. Its sections are never written. Its symbols and
  DWARF feed `m.sym`, `m.var`, the unwinder, coverage and trace. This is the secure ELF of a
  pair whose Non-secure ELF embeds the secure binary. `Image.elf(path, name=, load=False)` is
  the same image for `Machine.load`.
- `Machine.load` raises MachineBusy while a `trace` with call records is open.

At a stop, on the machine of the trustzone example:

```python
--8<-- "guide/test_trustzone.py:views"
```

Count arrivals per state:

```python
--8<-- "guide/test_trustzone.py:probe"
```

Registers and state:

- `m.security_state` is `"secure"`, `"nonsecure"`, or None without TrustZone. Writing
  `m.reg.secure` at a stop switches the state, as BXNS does.
- With TrustZone `m.reg.names()` ends with `secure` and the banked `msp_s`, `msp_ns`, `psp_s`,
  `psp_ns`, `control_s`, `control_ns`, `primask_s`, `primask_ns`, `basepri_s`, `basepri_ns`,
  `faultmask_s`, `faultmask_ns`, `msplim_s`, `msplim_ns`, `psplim_s` and `psplim_ns`. The plain
  names read and write the bank of the current state.
- A handler cannot write `secure`, `control_s`, `control_ns`, or `control` under TrustZone:
  code the core already translated would keep the old state, so the write raises
  MachineRunning. Without TrustZone a handler still writes `control`.
- `m.nvic.target_nonsecure(irq)` and `m.nvic.set_target(irq, nonsecure)` read and write
  NVIC_ITNS.
- A fault report decodes SFSR and shows SFAR for a SecureFault, `stop.fault.kind`
  `FaultKind.SECURE`.

Memory views. `m.mem` is the debugger view, unfiltered. `m.mem.secure` and `m.mem.nonsecure`
are a privileged debug access of that state, DDI0553B.z B13.3.4:

- The SAU and the IDAU attribute each access first. `m.mem.nonsecure` at a Secure or Non-secure
  callable address raises BusAccessError with `denied` True.
- The bus firewall then decides the access as a CPU access of those attributes: the GTZC TZSC,
  MPCBB and MPCWM4, the FLASH SECBB, PRIVBB and HDP, and the TrustZone-aware peripherals. A
  refused read reads 0, a refused write is dropped.
- A write to flash follows the rule of a read there. An allowed write lands in storage.
- No view access raises an illegal access event. The TZIC flags and GTZC_IRQn stay as they are.
- Both views are privileged. popcorn has no unprivileged view.
- A write of several bytes goes out one access at a time. When one raises BusAccessError, the
  bytes before it are written and the rest are not.
- Without TrustZone both views act as `m.mem`.
- `BusAccessError` is an EmulationError with `address` and `denied`. `denied` is False when
  nothing is mapped there.

Symbols, probes and calls:

- `m.sym`, `m.var`, `m.fn_ptr` and `m.image.var` take `image=`. Without it the image listed
  last wins a name both images hold, such as `main`. `Boot(image=)` picks the image of
  `until`.
- In a CMSE build an entry's name is its SG veneer, in both ELFs. `__acle_se_<name>` is the
  function behind it.
- `m.probe(target, state=)` with `"secure"` or `"nonsecure"` counts the arrivals in that state
  only. `state` on a machine without TrustZone is a ConfigError.
- `m.call` runs in the core's current state. An entry, a return sentinel or an arena part of
  the other state is a ConfigError before anything runs. An SG veneer is no entry from
  Non-secure state. A Non-secure call needs an arena the SAU makes Non-secure.

Snapshots and systems:

- A snapshot holds the security state, both register banks and the GTZC, FLASH and peripheral
  security state. A TZEN=1 snapshot is refused by a TZEN=0 machine.
- TrustZone is per machine. A linked `System` may mix TZEN=0 and TZEN=1 members.

Without TrustZone, every machine popcorn ran before keeps its behaviour, except these
readbacks of the STM32U5:

- FLASH `SECKEYR`, `SECSR`, `SECCR`, `SECBBRx` and `SECHDPCR` read 0 and ignore writes. `SECCR`
  no longer reads 0x80000000.
- FLASH `SECWMxRy` and `SECBOOTADD0R` read 0 and ignore writes, with or without `option_bytes`.
- `GPIOx_SECCFGR`, `RCC_SECCFGR`, `PWR_SECCFGR` and `SYSCFG_SECCFGR` read 0 and ignore writes.
  The GPDMA and LPDMA `RCFGLOCKR` ignore writes.

The [trustzone example](examples.md#trustzone) runs a secure and Non-secure pair
and lists every known deviation from the hardware.
