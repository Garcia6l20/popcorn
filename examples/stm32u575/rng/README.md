# A 64-byte key from the built-in RNG, LL

An STM32U575 application derives a 64-byte key the way a bootloader does at first boot. It
turns HSI48 on and waits for HSI48RDY, sets `RCC_AHB2ENR1.RNGEN`, enables the RNG with
`LL_RNG_Enable`, then reads 16 words with DRDY polled. Before keeping a word it checks SECS,
SEIS, CECS and CEIS, a word of 0 and a word equal to the one before. It stores the words and a
status in `rng_key`, turns the RNG and HSI48 off, then stops at `rng_done`:

| `rng_key.status` | meaning |
| --- | --- |
| 0 | 16 good words |
| 1 | seed error: SECS or SEIS, or a word of 0 |
| 2 | clock error: CECS or CEIS |
| 3 | two equal words in a row |
| 4 | DRDY did not rise within 2000 polls |

popcorn's built-in RNG model takes its words from a seed, 0 unless the description sets
`rng=RngOptions(seed=...)`, so the test predicts the key with `popcorn.board.rng_words`.
`m.periph.RNG.entropy` queues chosen words, sticks every word at one value and injects the seed
and clock errors. `conftest.py` sets `strict=True`.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/rng
```

## Read first

- `conftest.py`: the description, and a `seeded` config with `rng=RngOptions(seed=0x5EED)`
  served by the `rng_seeded` fixture.
- `test_rng.py`:
  - the key equals `rng_words(0, 16)` packed little-endian, run parked and inside
    `machine.running()`;
  - the `seeded` config gives `rng_words(0x5EED, 16)`;
  - `entropy.queue(...)` hands the firmware a chosen key;
  - `entropy.seed_fault_after = 8`: 8 good words, then SECS, status 1;
  - `entropy.seed_fault = True` and `entropy.clock_fault = True`: status 1 and 2, no word kept;
  - `entropy.stuck = 0x12345678`: status 3 at the second word;
  - `machine.watch(0x420C0808, kind=WatchKind.READ, ...)` sees each word the firmware reads;
  - `rng_use_hsi48` cleared before the run leaves HSI48 off: the RNG kernel clock is stopped and
    the clock checker sets CECS, status 2.
- `firmware/main.c`: the LL sequence and the checks.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/rng/rng_key.elf examples/stm32u575/rng/prebuilt/
```
