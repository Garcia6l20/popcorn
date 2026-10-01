# USB OTG_FS core bring-up, HAL PCD

An STM32U575 application starts HSI48, selects it as the 48 MHz ICLK, powers VDDUSB and brings
up the OTG_FS core in device mode with `HAL_PCD_Init`, sets the FIFO sizes and calls
`HAL_PCD_Start`. It records both HAL status codes and reaches `usb_core_ready`.

There is no USB traffic: no host attaches, nothing enumerates and the core raises no interrupt.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/otg_fs
```

## Read first

- `test_otg_fs.py`: runs to `usb_core_ready`, checks the HAL status codes and reads GRSTCTL
  through `machine.periph.OTG_FS`.
- `firmware/main.c`: the clock, `HAL_PCD_MspInit` and the PCD setup a CubeMX project generates.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/otg_fs/otg_fs_init.elf examples/stm32u575/otg_fs/prebuilt/
```
