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

Nothing is mapped at address 0. A firmware load or store through a NULL pointer stops the run with a
FAULT stop, `stop.fault.kind` `FaultKind.BUS` and `stop.fault.address` 0, PRECISERR and BFARVALID
set in `stop.fault.cfsr` and BFAR 0. An instruction fetch at 0 stops it the same way with IBUSERR.
`m.mem` at 0 raises `EmulationError`. On the STM32U5 this matches the hardware. 0x0000_0000 to
0x07FF_FFFF is the code-region window the ICACHE remaps onto external memories, RM0456 2.3 Table 3
and 8.4.7. Boot does not alias flash at 0: BOOT0 and the NSBOOTADDx and SECBOOTADD0 option bytes
give the address the core takes its initial SP and PC from, RM0456 chapter 4, and flash stays at
0x0800_0000. popcorn does not model the ICACHE remap: the ICACHE registers `CRR0` to `CRR3` store
what the firmware writes and map nothing, so address 0 stays unmapped even with a remap region
enabled. Parts that alias boot memory at 0, such as the STM32F4 and STM32L4, are not modelled.

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

## License

MIT, see `LICENSE`. Third-party components and their licences are listed in `NOTICE`.
