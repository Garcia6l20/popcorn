# popcorn

popcorn runs STM32 Cortex-M firmware on the Unicorn CPU emulator so it can be unit tested from
Python. A test loads the firmware ELF, calls single functions or boots the whole image with
emulated peripherals, drives inputs and asserts on outputs, memory, registers and peripheral
state. A pytest plugin boots once and restores a snapshot per test.

Peripheral registers carry their bare names, `m.periph.TIM1.CR1` and `m.periph.RCC.CR`. The
prefixed names ST's SVD gives them, `TIM1_CR1` and `RCC_CR`, stay valid as aliases through
`m.periph`, `m.ref.periph`, Python models and the built-in models. In the description itself,
`RegisterDesc.name` is the bare name and `RegisterDesc.aliases` holds the SVD name. Each
`m.description.peripherals` entry has `register(name)`, which finds a register by either name
and raises `KeyError` for neither:

```python
rcc = next(p for p in m.description.peripherals if p.name == "RCC")
assert rcc.register("RCC_CR").name == "CR"
```

## Address 0

Nothing is mapped at address 0. Under the default `faults="stop"`, a firmware load or store through
a NULL pointer stops the run with a FAULT stop, `stop.fault.kind` `FaultKind.BUS` and
`stop.fault.address` 0, PRECISERR and BFARVALID set in `stop.fault.cfsr` and BFAR 0. An instruction fetch at 0 stops it the same way with IBUSERR.
`m.mem` at 0 raises `BusAccessError`, an `EmulationError`, with `denied` False. On the STM32U5 this matches the hardware. 0x0000_0000 to
0x07FF_FFFF is the code-region window the ICACHE remaps onto external memories, RM0456 2.3 Table 3
and 8.4.7. Boot does not alias flash at 0: BOOT0 and the NSBOOTADDx and SECBOOTADD0 option bytes
give the address the core takes its initial SP and PC from, RM0456 chapter 4, and flash stays at
0x0800_0000. popcorn does not model the ICACHE remap: the ICACHE registers `CRR0` to `CRR3` store
what the firmware writes and map nothing, so address 0 stays unmapped even with a remap region
enabled. Parts that alias boot memory at 0, such as the STM32F4 and STM32L4, are not modelled.

## Faults

`faults=` on `MachineDescription` and `Machine` sets what a processor fault does:

```python
popcorn.MachineDescription(part="STM32U575QGIx", images=[popcorn.Elf(ELF)], faults="handler")
```

- `"stop"`, the default, ends the run with a FAULT stop at the faulting instruction.
  `stop.fault` holds the kind, the address and the fault status registers.
- `"handler"` enters the firmware's HardFault, MemManage, BusFault, UsageFault or SecureFault
  handler, as the core does, and the run goes on.
- Under both, lockup and an INVSTATE fault stop the run.
- Without TrustZone, INVPC, UNSTKERR, STKERR, a STKOF on entry and VECTTBL stop the run too.
- With TrustZone, a STKOF of the additional state context alone and a fault while taking a
  derived exception stop the run too.
- `faults` enters `MachineDescription.digest` only when it is not `"stop"`.

Fault classes and addresses:

- An alignment abort, such as an unaligned LDREX, is a UsageFault UNALIGNED on Armv7-M and
  Armv8-M, not a BusFault.
- A data abort reports the data address the access faulted at, not the PC.
- An access the bus firewall refuses with a bus error is a precise BusFault, BFAR its address.
  A bus error a peripheral model raises stays an imprecise BusFault with no BFAR.

## Host access to unmodelled data

A host access over the bus to data no model serves raises `ModelError` and logs nothing,
whatever `strict` says, with or without TrustZone. The register storage stays reachable:

```python
cr = m.periph.RNG.CR     # the STM32U575 RNG has no model
cr.read()                # ModelError
cr.poke(0x4)             # stores 0x4, no model involved
assert cr.value == 0x4   # reads the storage
m.mem.u32[0xE004_2000]   # ModelError, the CTI has no model
```

Host accesses over the bus are `m.mem` with its `secure` and `nonsecure` views, `m.ref.mem`,
and a register's `read()`, `write()` and `value` setter. Unmodelled data is:

- every register of a peripheral without a model
- an offset no register covers inside a modelled peripheral
- a register a model keeps as storage only, such as every SYSCFG register but `SECCFGR`
- the STM32U5 and STM32H5 debug blocks without a model: TPIU 0xE0040000, ETM 0xE0041000, CTI
  0xE0042000, the MCU ROM table 0xE00FE000 and the processor ROM table 0xE00FF000

The `value` getter, the field getters and `poke()` go to the register storage, without the
model. Firmware and DMA accesses follow other rules:

- A firmware access to a register without a model logs a model_warning once per register. Under
  `strict` it raises ModelError.
- A firmware write to a SYSCFG storage register lands and logs one model_warning per register.
  Under `strict` it raises ModelError before the store.
- A firmware or DMA access to a debug block without a model reads 0 and drops writes, with one
  model_warning per block. Under `strict` it raises ModelError naming the block. The ROM tables
  read 0, not their entries, so a ROM table walk ends at its first entry.

## TrustZone

popcorn runs the STM32U575 with TZEN=1. It models the Cortex-M33 Secure and Non-secure states,
the SAU and the IDAU, the banked SCB, NVIC, SysTick and MPU registers, SG, BXNS, BLXNS,
FNC_RETURN and SecureFault. On the STM32 side it models the GTZC TZSC, TZIC and MPCBB, the
flash secure areas, HDP and PRIVBB, the 0x5xxx secure aliases, and the `SECCFGR` and `PRIVCFGR`
bits of RCC, PWR, GPIO, EXTI, SYSCFG, FLASH, GPDMA and LPDMA. `OptionBytes(tzen=True)` turns it
on:

```python
popcorn.MachineDescription(
    part="STM32U575QGIx",
    images=[
        popcorn.Elf(HERE / "trustzone.elf", name="nonsecure"),
        popcorn.Elf(HERE / "trustzone_s.elf", name="secure", loaded=False),
    ],
    option_bytes=popcorn.OptionBytes(tzen=True, secwm1=(0, 3), secwm2=None),
    boot=popcorn.Boot(until="main", image="nonsecure"),
    faults="handler",
)
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

At a stop, on the machine of [examples/stm32u575/trustzone](examples/stm32u575/trustzone):

```python
assert m.security_state == "nonsecure"
assert m.reg.msp == m.reg.msp_ns           # the plain register follows the state
m.mem.u32[0x2002_FE00] = 0x1234            # a Secure MPCBB1 block, written unfiltered
assert m.mem.secure.u32[0x3002_FE00] == 0x1234
assert m.mem.secure.u32[0x2002_FE00] == 0  # the SAU makes this alias Non-secure
assert m.mem.nonsecure.u32[0x2002_FE00] == 0
try:
    m.mem.nonsecure.u32[0x3002_FE00]
except popcorn.BusAccessError as error:
    assert error.denied                    # a Secure address, refused to Non-secure
veneer = m.probe("secure_counter_add", state="nonsecure")
function = m.sym("__acle_se_secure_counter_add", image="secure")
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
- With `option_bytes` set and `tzen` False, `SECWMxRy` and `SECBOOTADD0R` read 0 and ignore
  writes.
- `GPIOx_SECCFGR`, `RCC_SECCFGR`, `PWR_SECCFGR` and `SYSCFG_SECCFGR` read 0 and ignore writes.
  The GPDMA and LPDMA `RCFGLOCKR` ignore writes.

[examples/stm32u575/trustzone](examples/stm32u575/trustzone) runs a secure and Non-secure pair
and lists every known deviation from the hardware.

## Install

popcorn is installed from source, the native core is compiled on your machine.

Prerequisites on Linux x86_64:

- Python >= 3.14 with its development headers, and uv
- g++ >= 16.1 as `g++`
- cmake and ninja
- git and network access: Unicorn and reflex are git submodules, the other third-party sources
  are fetched at build time
- optional: arm-none-eabi-gcc, to rebuild the test firmware

```sh
uv pip install git+<repository-url>
```

Two extras add optional dependencies. `pytest` installs pytest for the `popcorn.pytest` plugin,
`plugin` installs pcons and ninja for dev-mode native plugins, `popcorn.plugin`:

```sh
uv pip install "popcorn[pytest] @ git+<repository-url>"
uv pip install "popcorn[pytest,plugin] @ git+<repository-url>"
```

For local development, clone with `--recurse-submodules`: Unicorn and reflex under `third_party/`
are git submodules, and the build reads the Unicorn commit from git. A copy of the tree without its
git metadata is not supported.

```sh
git clone --recurse-submodules <repository-url>
# or, on an existing clone:
git submodule update --init
```

## Build variables

The package build, `uv sync` or `pip install`, compiles the package only: `[tool.pcons]` in
`pyproject.toml` sets both variables below to `OFF`. A pcons build defaults both to `ON`, but a
build directory keeps the values it was last configured with, and the package build configures
`build/` with both `OFF`.

| Variable | Default | `OFF` skips |
| --- | --- | --- |
| `POPCORN_BUILD_TESTS` | `ON` | native tests, test firmware, the header check, `popcorn_testing._smoke`, the firmware packages of `deps.toml`, but doctest is still fetched |
| `POPCORN_BUILD_EXAMPLES` | `ON` | example firmware, and the firmware packages when tests are off too |

Each takes `ON`, `OFF`, `1` or `0`, in any case. Anything else stops the configure.

pcons keeps a `KEY=value` variable in the build directory, and the package build uses `build/`.
After a `uv sync`, or any `uv run` that re-syncs the editable package, pass both `ON` once to get
the tests and examples back:

```sh
uv run --no-sync pcons -B build POPCORN_BUILD_TESTS=ON POPCORN_BUILD_EXAMPLES=ON
```

`pcons run check` passes both `ON` itself.

## Native plugins

A native plugin adds C++ peripheral models and C++ bus devices, built against popcorn's C++ API
and loaded at run time. A machine description declares it and selects its models by name:

```python
from popcorn.plugin import Plugin

PLUGIN = Plugin.dev(HERE / "plugin")
popcorn.MachineDescription(
    ...,
    plugins=[PLUGIN],
    board={"RNG": popcorn.Periph(model=PLUGIN.model("rng"))},
)
```

A guest access to a plugin model stays native. Measured on an AMD Ryzen AI 7 PRO 350, Python 3.14.8, GCC 16.2.1, release `-O2` build,
a guest loop of 10^6 RNG DR reads with DRDY clear, the cheapest model path, three sessions of
11 runs, session medians: 53.5 to 54.8 ns per read with the plugin model, 1150 to 1179 ns with
the Python model, 38.4 to 39.5 ns for an RNG register no model claims. A whole run of the
python_model firmware to its 8th word took 0.63 to 0.72 ms with the plugin model, 0.89 to
1.11 ms with the Python model.

Of the 37 ns every guest MMIO access pays, the access trace record takes most. A plugin read
adds 15 ns of dispatch. A Python model read adds about 110 ns to take and release the GIL,
80 ns to enter Python and about 870 ns of interpreted code: `Model` dispatch, then the
handler's `regs` and `fields` lookups. A whole run gains less because Unicorn translating the
firmware into a fresh machine takes half of it, and each of the run's 56 Python calls costs
about 3 us on a machine thread that starts with cold caches. The bench
[tests/python/native_plugin/bench_native_model.py](tests/python/native_plugin/bench_native_model.py)
measures both, run it with `POPCORN_BENCH=1 uv run python -m pytest -s` on that file.

`Plugin.dev` builds a source directory on first use, which needs the `popcorn[plugin]` extra
and the GCC 16 build popcorn was built with. `popcorn.plugin.find` returns a plugin installed
from a wheel. [examples/stm32u575/native_plugin](examples/stm32u575/native_plugin) shows a model
and a device, and documents dev and packaged builds and the rules a plugin follows.

## License

MIT, see `LICENSE`. Third-party components and their licences are listed in `NOTICE`.
