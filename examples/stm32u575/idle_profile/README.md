# Idle time from FreeRTOS, with and without WFI in the idle hook

An STM32U575 application on the 16 MHz HSI runs FreeRTOS at a 1 kHz tick with two polling
tasks. `status_task` starts a 2 ms TIM16 one-pulse every 100 ms and polls TIM16_SR until UIF.
`flag_task` sleeps 240 ms, then spins on `timer_flag`, a RAM flag the 4 Hz TIM2 interrupt sets.
The rest of the time belongs to the idle task. One source builds two images: `idle_wfi.elf`
with `IDLE_WFI`, whose `vApplicationIdleHook` calls `__WFI()`, and `idle_spin.elf` without it.

`Machine.profile` reports one virtual second of each. With the hook the core sleeps over 90 % of
the time, woken by SysTick and the TIM6 HAL tick. Without it the idle task's loop spins over
90 % of the time. In both, the loops section shows the two poll shapes: the status poll ends
when the TIM16_SR value it reads changes, the flag poll keeps the same registers on every pass,
so only an exception, or the end of the profile, ends its runs.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/idle_profile
```

Print a whole report from a test with `print(profile.report().text())`, or profile every test of
the example with the pytest option, which writes one JSON report per test and machine to
`popcorn-profile/` in the example directory:

```sh
uv run pytest -q examples/stm32u575/idle_profile --popcorn-profile
```

## Read first

- `conftest.py`: one description per image, `idle_wfi` and `idle_spin`, both stopped at `main`.
- `test_idle_profile.py`: a profile over one virtual second, the sleep share, the loop runs of
  the two polls and the TIM16_SR reads.
- `firmware/main.c`: the two tasks, the TIM2 interrupt and the idle hook.
- `firmware/FreeRTOSConfig.h`: `configUSE_IDLE_HOOK` from `IDLE_WFI`.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/idle_profile/idle_*.elf examples/stm32u575/idle_profile/prebuilt/
```

Without the toolchain, the build uses the committed `prebuilt/idle_wfi.elf` and
`prebuilt/idle_spin.elf`.
