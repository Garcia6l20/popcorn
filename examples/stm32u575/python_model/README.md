# An unmodelled peripheral served by a Python model: the RNG, HAL IT

An STM32U575 application turns HSI48 on, initialises the RNG with `HAL_RNG_Init`, then asks
for eight random words, one `HAL_RNG_GenerateRandomNumber_IT` at a time. Each word arrives in
`HAL_RNG_ReadyDataCallback`, which stores it in `rng_result`. The main loop sleeps in WFI
while it waits.

popcorn has no RNG model. Without one, the RNG registers are plain storage: DRDY never sets,
and a strict machine stops on the first RNG access. `rngmodel.Rng` is a `popcorn.model.Model`
written next to the test that serves CR and DR: RNGEN starts a conversion of 40 us of virtual
time, DRDY and the RNG interrupt follow, and each DR read returns the next word of a seeded
xorshift32 sequence. A CONDRST write cancels the conversion in progress and clears SR,
clearing RNGEN stops production and keeps DRDY. The model docstring lists what it simplifies
and leaves out.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/python_model
```

## Read first

- `rngmodel.py`: the whole model. `@on_write("CR")` and `@on_read("DR")` claim the two
  registers, every other RNG register keeps its storage. `self.schedule` runs a method after a
  virtual delay and returns a handle that `self.cancel` takes. `self.irq`, an `IrqLine`
  field, drives the interrupt. `self.regs` and `self.fields` read and write registers and
  fields by name. The annotated fields are the state snapshots save and a reset restores.
- `conftest.py`: `board={"RNG": Periph(model=Use(Rng))}` puts the model in the RNG slot.
  `with_configs(seeded=...)` passes `seed=7` to its constructor.
- `test_python_model.py`:
  - a machine built without the model, strict, raises ModelError naming RNG;
  - the words the firmware received equal the seeded sequence, computed in the test from
    `rngmodel.words`, and `machine.model("RNG")` returns the model;
  - `HAL_RNG_Init` writes RNG_NSCR, a register ST's SVD omits and popcorn's description
    adds, so no unmodelled-register warning is logged;
  - an intercept on `HAL_RNG_ReadyDataCallback` shows each later word one conversion after
    the last;
  - the `seeded` config gives another sequence.
- `firmware/main.c`: the HSI48 and RNG init, the interrupt-driven requests.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/python_model/rng_numbers.elf examples/stm32u575/python_model/prebuilt/
```
