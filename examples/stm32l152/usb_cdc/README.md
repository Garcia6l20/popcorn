# USB device enumerated by a virtual host, TinyUSB

An STM32L152 application runs TinyUSB 0.21.0 on the STM32L1 USB full-speed device controller,
RM0038 chapter 24: a CDC ACM function that echoes what it receives upper-cased, once the host
has set DTR. The firmware runs the core at 32 MHz from an 8 MHz HSE bypass through the PLL, x12
and /3, so the 96 MHz PLL VCO gives the 48 MHz USB clock. It enables the SYSCFG and USB clocks,
calls `tusb_init` and runs `tud_task` in a main loop that sleeps in WFI. TinyUSB's `stm32_fsdev`
driver serves the controller, keeps its endpoint buffers in the 512-byte packet memory and
switches the D+ pull-up on through SYSCFG_PMC.USB_PU. Descriptors use TinyUSB's example values:
VID 0xCafe, PID 0x4001, strings `TinyUSB` and `TinyUSB Device`.

It ports `stm32h573/usb_cdc`. The STM32L1 controller has no BCDR, so the pull-up is in SYSCFG,
and its packet memory is a block of its own, `USB_SRAM`. The board entry is `USB`.

`Periph(host=UsbHostOptions())` on USB in the description gives the controller model a virtual
full-speed host with its cable plugged from power-on. The host sees the pull-up, waits the
100 ms debounce, resets the bus and enumerates the device as Linux does, in virtual time,
without Python on the transaction path. Its state is in snapshots, so a snapshot taken once
configured restores a configured device.

```python
rcc=RccOptions(hse_hz=8_000_000),
board={
    "USB": popcorn.Periph(host=UsbHostOptions()),
},
```

The host's CDC ACM driver binds the CDC function at configuration: SET_LINE_CODING 115200 8N1,
then SET_CONTROL_LINE_STATE with DTR and RTS. `host.cdc()` is the function as a byte stream,
`machine.board.pty("USB")` bridges it to a host pty any serial tool opens.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l152/usb_cdc
```

## Read first

- `test_usb_cdc_l152.py`: `machine.periph.USB.host` is the virtual host. `wait_configured` runs
  the machine until the device is configured, `device_descriptor`, `strings` and `interfaces`
  give what the host read, compared with the descriptors in the firmware through `machine.var`,
  and `control` sends a request of the test's own. The controller registers show the address,
  the endpoint numbers the driver set and USB_PU, and RCC the PLL and the clock enables.
  `host.cdc()` writes and reads the echo, across a snapshot, an unplug and a replug, parked and
  inside `running()`, and through a pty.
- `firmware/usb_descriptors.c`: the device, configuration and string descriptors.
- `firmware/main.c`: the clock setup and the CDC echo. PA11 and PA12 need no GPIO setup:
  DS10002 Table 8 gives USB_DM and USB_DP as additional functions, which the USB peripheral
  selects itself, and Table 9 has no USB alternate function.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH, which also fetches TinyUSB:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m3/examples/stm32l152/usb_cdc/usb_cdc.elf examples/stm32l152/usb_cdc/prebuilt/
```

The build links `../STM32L152XE_FLASH.ld`, 256 KB flash and 32 KB SRAM, so the stack lies inside
the SRAM of every part it runs on.
