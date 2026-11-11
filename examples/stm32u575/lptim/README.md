# LPTIM1 on MSIK with an autoreload and a compare match, LL

An STM32U575 application runs LPTIM1 on MSIK at 4 MHz divided by 128, one count every 32 us. With
ARR 3124 the counter wraps every 100 ms. CC1IF sets at CCR1 1562 and ARRM at ARR, 49.984 ms
apart. The ARRM interrupt counts `periods` and toggles PA5, the CC1IF interrupt counts
`compares`. The main loop sleeps in WFI.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/lptim
```

## Read first

- `test_lptim.py`:
  - MSIKRDY, LPTIM1SEL, LPTIM1EN, ARR, CCR1, ENABLE and PRESC after the start;
  - an intercept on `LPTIM1_IRQHandler`: the interrupts alternate compare then reload, each
    kind exactly 100 ms apart, each reload 49.984 ms after its compare;
  - PA5 read after each period.
- `conftest.py`: the boot stops at `lptim_started`, after the counter starts, with
  `MachineDescription(strict=True)`.
- `firmware/main.c`: the MSIK, GPIO and LPTIM1 setup and the interrupt handler. Each DIER, ARR
  and CCR1 write waits for its DIEROK, ARROK or CMP1OK flag before the next one.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/lptim/lptim_msik.elf examples/stm32u575/lptim/prebuilt/
```
