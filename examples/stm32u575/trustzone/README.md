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
  ```

- `test_trustzone.py`: one behaviour per test. Each test selects a case of the Non-secure
  `main` through `ns_case`, set with `m.var("ns_case", image="nonsecure").value` and the
  enumerator from `m.image.constants`. The tests cover:
  - `m.security_state`
  - `m.probe(state=)` on the veneer and on the function behind it
  - PA5 driven from Non-secure code
  - the RAZ read and GTZC interrupt of a Secure MPCBB block
  - the two SysTick timers
  - the FP context across BLXNS
  - a snapshot taken inside a Non-secure callback, loaded into a new machine
  - `m.sym(image=)` and the embedded secure bytes
- `firmware/secure/main.c`: the setup, the entry functions, `GTZC_IRQHandler`,
  `SecureFault_Handler`.
- `firmware/nonsecure/main.c`: the cases, and `fp_clobber.s`, a callback that overwrites S0
  to S31.
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
