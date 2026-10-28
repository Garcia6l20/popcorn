# Secure and Non-secure pair with TrustZone, CMSE

An STM32U575 boots with TZEN=1 into a secure part that sets up the SAU, the GTZC and the
Non-secure vector table, then enters a Non-secure application with BLXNS. The Non-secure ELF is
the only image a flasher writes: it embeds the secure binary with `.incbin`. The secure ELF is
loaded for its symbols only.

| part | flash | SRAM |
| --- | --- | --- |
| secure | 0x08000000-0x08007BFF, at the 0x0800 alias | SRAM4 0x28000000-0x28003FFF through the Non-secure alias, MPCBB4 left Secure |
| NSC veneers | 0x08007C00-0x08007FFF, an SAU NSC region | - |
| Non-secure | 0x08008000-0x0803FFFF | SRAM1 0x20000000-0x2002FDFF, the last MPCBB1 block 0x2002FE00 kept Secure |

The secure part runs these steps in order:

- `SystemInit` enables CP10 and CP11 for both states and sets FPCCR_S.TS.
- `main` points VTOR_S at its vector table and enables SecureFault.
- `main` opens SAU regions over the veneers, the Non-secure flash, SRAM1 and the peripherals, but
  leaves out the FLASH register block.
- `main` releases every MPCBB1 block but the last and enables the TZIC SRAM1 interrupt.
- `main` releases PA5, sets NSACR CP10 and CP11, and sets AIRCR.BFHFNMINS.
- `main` sets VTOR_NS and MSP_NS from the Non-secure vector table, then calls the Non-secure reset
  handler through a `cmse_nonsecure_call` pointer.

The secure part is built `-mfloat-abi=softfp`, so `__gnu_cmse_nonsecure_call` saves its FP
context with VLSTM and VLLDM. The Non-secure part is hard-float. The entry functions take and
return integers only, so the two ABIs never pass FP values to each other.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/trustzone
```

## Read first

- `conftest.py`: the two images, one named and loaded, one symbols-only, the TrustZone option
  bytes and the boot to the Non-secure `main`:

  ```python
  images=[
      popcorn.Elf(HERE / "prebuilt/trustzone.elf", name="nonsecure"),
      popcorn.Elf(HERE / "prebuilt/trustzone_s.elf", name="secure", loaded=False),
  ],
  option_bytes=popcorn.OptionBytes(tzen=True, secwm1=(0, 3), secwm2=None),
  boot=popcorn.Boot(until="main", image="nonsecure", budget=100_000),
  faults="handler",
  ```

  `faults="handler"` makes a fault enter the firmware's handler, where the default stops the
  run at the faulting instruction.

- `test_trustzone.py`: one behaviour per test. Each test selects a case of the Non-secure
  `main` through `ns_case`, set with `m.var("ns_case", image="nonsecure").value` and the
  enumerator from `m.image.constants`. The tests cover:
  - `m.security_state`
  - `m.probe(state=)` on the veneer and on the function behind it
  - PA5 driven from Non-secure code
  - the RAZ read and GTZC interrupt of a Secure MPCBB block
  - SecureFault AUVIOL with SFAR, and INVEP for a call past the veneer
  - the HardFault each state's escalated UsageFault takes with AIRCR.BFHFNMINS set
  - the two SysTick timers
  - the FP context across BLXNS
  - a snapshot taken inside a Non-secure callback, loaded into a new machine
  - `m.sym(image=)` and the embedded secure bytes
- `firmware/secure/main.c`: the setup, the entry functions, `GTZC_IRQHandler`,
  `SecureFault_Handler` recording SFSR and SFAR, the Secure `HardFault_Handler`.
- `firmware/nonsecure/main.c`: the cases and the Non-secure `HardFault_Handler`, and
  `fp_clobber.s`, a callback that overwrites S0 to S31.
- `firmware/secure_entry.h`: the entry functions both parts share.

In a CMSE build an entry's name is its SG veneer, in the import library and in the secure ELF
alike. `__acle_se_<name>` is the function behind the veneer:

```python
m.sym("secure_counter_add", image="nonsecure")            # 0x08007c20, the veneer
m.sym("__acle_se_secure_counter_add", image="secure")     # 0x08000390, the function
```

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/trustzone/trustzone{,_s}.elf \
   examples/stm32u575/trustzone/prebuilt/
```

`pcons-build.py` passes the layout to `popcorn_build.examples.add_trustzone_example_firmware`.
The build derives both linker scripts from ST's `STM32U575xx_FLASH_s.ld` and `_ns.ld`. It links
the secure ELF with `--cmse-implib`, runs `objcopy -O binary` on it, and links the Non-secure ELF
against the import library and the embedded binary. `firmware-check` also fails when the
Non-secure ELF's `.secure_image` differs from the secure ELF's binary. Without the toolchain, the
build uses the committed `prebuilt/` ELFs.

The CMSE import library is not a declared output of the build. Deleting it alone makes the
Non-secure link fail until the secure ELF is rebuilt.

## Known deviations

Where popcorn differs from the STM32U575 or the Armv8-M architecture. Each item names the case
that pins it, by file and the first words of its title, or says why no case does. Native cases
are under `tests/native`, Python cases under `tests/python/api`.

Not modelled:

- Option-byte programming. Option bytes come only from `OptionBytes`. OPTSTRT is cleared and
  logs one model_warning under TrustZone, so firmware that reprograms SECWM and reloads cannot
  run its reprogram path. `stm32/test_flash_secure.cpp` "SECCR.INV is stored, and OPTSTRT".
- `OptionBytes(dualbank=False)` is refused. On the 1 MB STM32U575 it is the single-bank
  organization, whose geometry the FLASH model does not lay out. Boot addresses are checked for
  128-byte alignment only, RM0456 Table 27 allows any address at RDP level 0.
  `test_option_bytes.py`
  `test_a_value_the_u5_family_refuses_raises_before_the_machine_is_built`.
- Without `option_bytes`, FLASH_OPTR reads 0, the description's reset, not the production
  0x1FEFF8AA. With `option_bytes` the reserved bits read their production values.
  `stm32/test_flash.cpp` "without option bytes the option registers are the description's
  storage" and `test_option_bytes.py` `test_tzen_1_reads_the_encoded_option_bytes`.
- Privilege filters without TrustZone. TZSC `PRIVCFGRx`, MPCBB `PRIVCFGRx` and FLASH `PRIVBBRx`
  filter only under TZEN=1. Under TZEN=0 they are storage. `stm32/test_gtzc.cpp` "without
  TrustZone no GTZC block has a model".
- MPU enforcement. The MPU is storage only, so TT reports MRVALID 0 with an MPU region
  programmed, and SG checks no MPU region. `arch/test_scs_security.cpp` "TT reports MRVALID 0".
- RDP level 0.5 and the external-memory watermarks MPCWM1 to MPCWM3, MPCWM5 and MPCWM6. popcorn
  has no OCTOSPI or FMC model. Not tested, nothing to reach.
- Execution from BKPSRAM. MPCWM4 filters data accesses only: BKPSRAM is execute-never in the
  default memory map, so no fetch reaches the filter. `stm32/test_gtzc.cpp` "an MPCWM4 store
  that makes a BKPSRAM sub-region Non-secure".
- RTC and TAMP `SECCFGR` and `PRIVCFGR`. They are storage and log one model_warning each under
  TrustZone. RCC BDRST and the RTC clock bits follow the stored secure functions.
  `stm32/test_tz_aware.cpp` "Q9: RTC_SECCFGR and TAMP_SECCFGR stay storage" and "RM0456 11.5.1:
  BDRST and the RTC clock bits".
- RCC bits that name no peripheral of the part stay Non-secure: SRAMnEN, SRAMnSMEN, BKPSRAM,
  GTZCn, AES, SAES and SHSI. The MPCBB INVSECSTATE rule of RM0456 5.4 is not modelled. Not
  tested, no rule to pin.
- A kernel clock selection shared by several peripherals, such as SDMMCSEL, OCTOSPISEL,
  ADCDACSEL, LPTIM34SEL and TIMICSEL, is Secure while any of them is. `stm32/test_tz_aware.cpp`
  "RM0456 11.5.1: a kernel clock selection shared by several peripherals is Secure while any of
  them is".
- SYSCFG `CSLCKR` and `CNSLCKR` are stored and lock nothing. `RSSCMDR` and the OTG_HS PHY
  registers have no security rule. Under TZEN=0 `CSLCKR` is storage, where RM0456 Table 131
  makes it RAZ/WI. Not tested, storage only.
- The flash bus stall of RM0456 7.3.5 while an operation is busy. `stm32/test_flash_secure.cpp`
  "a Non-secure sequence error while a Secure page erase is BSY".
- The core state under TZEN=0. The engine keeps running Secure, so BXNS and BLXNS switch it to
  Non-secure and the next fetch takes a MemManage. RM0456 states nothing on BXNS and BLXNS with
  TZEN=0. Not tested, the silicon result is unknown.

Register rules:

- Without TrustZone the GTZC TZSC `SECCFGRx`, `PRIVCFGRx` and `CR` and every MPCBB register are
  storage at the SVD reset with no model. RM0456 3.5.7 makes the secure registers of a
  TrustZone-aware peripheral RAZ/WI then, so the MPCBB `SECCFGRx` and `PRIVCFGRx` read
  0xFFFFFFFF where silicon reads 0. `stm32/test_gtzc.cpp` "without TrustZone no GTZC block has
  a model".
- Without TrustZone `EXTI_SECCFGR1` and the GPDMA and LPDMA `SECCFGR` stay storage. RM0456
  states no TZEN=0 rule for them. `stm32/test_tz_aware.cpp` "EXTI_SECCFGR1 stays storage under
  TZEN=0" and "RM0456 17.8.3, P1: DMA RCFGLOCKR".
- A `GPIOx_SECCFGR` write lands with the port clock off. RM0456 states no rule. Not tested.
- A GPDMA `CxTR1` write while the channel is enabled lands, where RM0456 17.8.10 makes it
  read-only. `stm32/test_tz_aware.cpp` "popcorn takes a CxTR1 write while EN".
- A peek or poke of a channel's `SECCFGR`, `PRIVCFGR` or `CxTR1` reaches the channel at its
  next transfer, not during the current one. Not tested, host storage access only.
- MPCBB registers of super-blocks past the SRAM read 0 and drop writes. `stm32/test_gtzc.cpp`
  "RM0456 5.3 Table 33: the U575 MPCBB1 has 12 super-blocks".
- A Non-secure read of the TZSC `CR` or `CFGLOCKRx` raises the illegal access event of RM0456
  3.5.5. RM0456 5.5 states it only for the TZIC. `stm32/test_gtzc.cpp` "RM0456 5.7: TZIC
  registers are Secure privileged only" pins the TZIC. The `CR` and `CFGLOCKRx` event is not
  tested.
- A TZSC or TZIC that a Python or plugin model replaces turns its checks off: its peripherals are
  Non-secure and its events are dropped. Not tested, documented in the TZSC class doc.
- FLASH has one write buffer for both register sets. A store of the other set into a filling
  buffer is a PGSERR of the store's set, a rule RM0456 does not state.
  `stm32/test_flash_secure.cpp` "RM0456 7.3.9: the Non-secure STRT set by a Secure access".
- A flash store of the wrong security with PG clear sets WRPERR, not PGSERR: the security gate
  runs first, following Table 68. Not tested with PG clear.
- An unprivileged fetch of a PRIVBB page is a BusFault, where RM0456 Table 73 says it reads 0.
  `stm32/test_flash_secure.cpp` "RM0456 7.5.6 and Table 73: a PRIVBB page reads 0".
- FLASH `NSPRIV` applies only to the registers the model serves. `OPTKEYR`, `PDKEYxR`, `OPSR`,
  `WRPxyR` and the OEM keys stay unclaimed storage. Not tested, storage only.

Bus filter:

- The reset vector reads are not filtered. A Secure boot address in a Non-secure page reads its
  vectors where silicon reads 0. Not tested, a boot that silicon refuses.
- A fetch raises the illegal access event only when it is a Secure fetch of a Non-secure SRAM
  block or BKPSRAM sub-region. A Non-secure fetch, any flash fetch and an unprivileged fetch only
  BusFault. `stm32/test_gtzc.cpp` "RM0456 5.4.2: a fetch the MPCBB refuses is a BusFault".
- A Non-secure DMA read of Secure SRAM returns 0 and a write is dropped, with the TZIC event and
  no DTEF. `stm32/test_tz_aware.cpp` "RM0456 17.4.14, 5.4.2: a Non-secure channel reads Secure
  SRAM as zeros".
- The filtered views `m.mem.secure` and `m.mem.nonsecure` raise no illegal access event, and
  both are privileged. `test_trustzone_api.py`
  `test_no_view_access_raises_an_illegal_access_event`.

Floating point:

- A plain FP instruction never raises NOCP from CPACR, whatever CPACR_S or CPACR_NS says. Only
  VLSTM and VLLDM check CPACR, through CPACR_S. `arch/test_unicorn_security.cpp` "an FP
  instruction in Secure state sets CONTROL.FPCA" and `arch/test_exceptions_security.cpp` "a
  VLSTM with CPACR_S CP10 0 is a Secure UsageFault NOCP".
- A Non-secure FP instruction with NSACR.CP10 0 raises UsageFault_S NOCP. With CPACR_NS.CP10 0
  too it still targets UsageFault_S, where the architecture checks CPACR_NS first and targets
  UsageFault_NS. `arch/test_exceptions_security.cpp` "a Non-secure FP read of S0 with
  CPACR_NS.CP10 granted".
- No lazy FP preservation. The engine forces FPCCR.LSPEN to 0, so every VLSTM and every
  exception entry saves the FP context at once. FPCCR.LSPEN reads back what firmware wrote,
  FPCAR is storage only. The fork case `test_arm_v8m_fp_context` and
  `arch/test_scs_security.cpp` "FPCCR writes reach the engine by the D1.2.100 table".
- FPCCR.LSPACT is RAZ/WI, so the LSERR rules of exception entry and return and the VLSTM LSERR
  are unreachable from firmware. `arch/test_scs_security.cpp` "FPCCR writes reach the engine".
- GCC hard-float CMSE calls save d8 to d15 with VPUSH, not VLSTM, a toolchain fact. The secure
  part of this example is softfp so that its calls run VLSTM and VLLDM.
  `arch/test_exceptions_security.cpp` "a hard-float cmse_nonsecure_call saves d8-d15 with
  VPUSH".

Exceptions, under `faults="handler"`:

- The integrity faults of an exception return, SFSR INVER, INVIS and AUVIOL, UFSR INVPC and
  BFSR UNSTKERR, are taken on the frame left in place, then the best pending exception
  tail-chains in. The entry faults AUVIOL, STKOF and STKERR, the NOCP of a Non-secure entry with
  NSACR.CP10 0 and VECTTBL are taken as derived exceptions. `arch/test_exceptions_security.cpp`,
  the cases from "a corrupt integrity signature on return to Secure" to "a bus error unstacking
  a Non-secure frame".
- These still stop the run: lockup, a STKOF of the additional state context alone, and any
  fault while taking the derived or chained exception, where silicon recurses into
  DerivedLateArrival() and may lock up. `arch/test_exceptions_security.cpp` "an INVER return
  that cannot take a fault under FAULTMASK_S" and "a failed NMI vector read locks up".
- An entry STKOF or AUVIOL stores no part of the frame, one of the orders Stack() allows. After a
  return fault the registers keep their values from before the return, where PopStack() has
  loaded part of the frame. Not tested: the cases check the SP and the fault taken, not the
  frame memory or the registers.
- A return clears the active bit of its IPSR number. DeActivate() clears HardFault or NMI when
  the raw execution priority is negative, which differs only for a HardFault or NMI handler that
  returns with a forged Handler-mode IPSR. Not tested, no firmware returns that way.
- A failed vector read takes the HardFault of the state of the exception being fetched, B3.11
  RDXYK. The MemA pseudocode targets Non-secure whenever BFHFNMINS is 1, which differs for a
  Secure exception. `arch/test_exceptions_security.cpp` "a failed vector read takes the
  HardFault of the state".
- A Non-secure entry with CONTROL.FPCA set and NSACR.CP10 0 stacks a basic frame with no FP
  register and takes the derived UsageFault_S NOCP. When that frame also takes AUVIOL, STKERR
  or STKOF, popcorn sets UFSR_S.NOCP and takes the frame's fault, one of the two choices
  PushStack() allows. `arch/test_exceptions_security.cpp` "a Non-secure IRQ over Non-secure code
  with FPCA set" and "a Non-secure entry with FPCA set and NSACR.CP10 0 that faults".
- A return to Non-secure with EXC_RETURN.FType 0 and NSACR.CP10 0 takes UsageFault_S NOCP on the
  frame left in place. popcorn tail-chains nothing before PopStack(), so an exception silicon
  takes on that return is taken after this fault. CPACR_NS is not checked there.
  `arch/test_exceptions_security.cpp` "a return to Non-secure with FType 0 and NSACR.CP10 0".
- After a derived STKOF from a Secure FP background, a Secure handler sees S0 to S31 and FPSCR
  live, where PushStack() zeroes them. Not tested, no such path reaches a Non-secure handler.
- When only the FP part of a return frame is denied or unreadable, popcorn raises AUVIOL or
  UNSTKERR where silicon raises NOCP first: the frame is read in one block. Not tested.
- A STKERR or VECTTBL stop over a Secure FP background with FPCCR_S.TS 1 reports S0 to S31 and
  FPSCR as 0: the clearing runs before the stop. The stacked frame still holds the values.
  `arch/test_exceptions_security.cpp` "a bus error stacking a Secure FP frame".

Secure calls:

- The STKOF of a BLXNS is taken as a derived Secure UsageFault with MSP_S or PSP_S at the limit.
  `arch/test_sg.cpp` "BLXNS with MSPLIM_S above its frame".
- A FNC_RETURN frame that cannot be read raises BusFault UNSTKERR with the branch, as silicon
  does. A frame not 8-byte aligned, UNPREDICTABLE, raises the Secure UsageFault INVPC.
  `arch/test_sg.cpp` "a FNC_RETURN frame that cannot be read" and "a FNC_RETURN frame not
  8-byte aligned".
- An intercept on a veneer that a Non-secure handler tail-calls with `b`, LR still an
  EXC_RETURN, completes the exception return in the trap. A return hook or an `until_return`
  waiting on that handler's return is not completed there. `arch/test_sg.cpp` "an intercept
  returning at an SG veneer to a FNC_RETURN link" pins the return in the trap for a FNC_RETURN
  link. The EXC_RETURN link is not tested.
- An emulated SG counts as a one-instruction coverage block with no edge from the caller and
  no block trace record. Not tested: no case reads coverage or the block trace at an SG.
- SG reads its halfwords through the debugger path. A failed read gives INVEP, not BusFault
  IBUSERR, and no MPU region is checked, the MPU being storage only. Not tested: an NSC region
  over unmapped memory is the only way to fail the read.

System space:

- The host, a Secure privileged master, reaches SCS_NS at 0xE002E000, which the architecture
  makes RES0 to a debugger. Firmware cannot observe it. `arch/test_scs_security.cpp` "a Secure
  store to SCS_NS lands in VTOR_NS".
- DEMCR.SDME is read-only with TrustZone. DEMCR MON_EN, MON_PEND, MON_STEP and MON_REQ are not
  RAZ/WI to Non-secure while SDME is 1. `arch/test_scs_security.cpp` "DEMCR.SDME hides the
  DebugMonitor priority".
- A Non-secure read of ICSR.VECTPENDING shows a pending Secure exception, the Armv8.0-M rule of
  the STM32U5's core. Armv8.1-M reads 1 there. Not tested, no Armv8.1-M part is modelled.

Debug blocks:

- The TPIU, ETM, CTI and both ROM tables have no model. They read 0 and drop writes with one
  model_warning per block, and raise ModelError under `strict` and from the host.
  `stm32/test_gtzc.cpp` "RM0456 75.5: the U5 debug blocks without a model" and
  `test_debug_blocks.py`.
- The ROM tables read 0, not the RM0456 Tables 797 and 798 entries, so a ROM table walk ends at
  its first entry. Pinned by the zero reads only.
- The exact-pc memory hook range spans the debug blocks, up to 0xE00FFFFF. While a memory trace
  sink or a watch holds that hook, every SCS, NVIC and SysTick access pays it too. Not timed
  yet.
