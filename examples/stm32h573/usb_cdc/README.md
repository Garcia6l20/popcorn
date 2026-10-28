# USB device enumerated by a virtual host, TinyUSB

An STM32H573 application runs TinyUSB 0.21.0 on the USB full-speed device controller, the USB
DRD FS of RM0481 chapter 61, in device mode: a CDC ACM function that echoes what it receives
upper-cased, once the host has set DTR. The firmware starts HSI48 as the USB kernel clock,
powers VDDUSB, puts DM and DP on PA11 and PA12 AF10, calls `tusb_init` and runs `tud_task` in a
main loop that sleeps in WFI. TinyUSB's `stm32_fsdev` driver serves the controller. Descriptors
use TinyUSB's example values: VID 0xCafe, PID 0x4001, strings `TinyUSB` and `TinyUSB Device`.

It ports `stm32u575/usb_cdc`. The H5 has the USB DRD FS controller in place of the OTG_FS core,
so the board entry is `USB` and the driver keeps its endpoint buffers in the packet memory. The
firmware carries a single CDC ACM function.

`Periph(host=UsbHostOptions())` on USB in the description gives the controller model a virtual
full-speed host with its cable plugged from power-on. The host sees the pull-up, waits the
100 ms debounce, resets the bus and enumerates the device as Linux does, in virtual time,
without Python on the transaction path. Its state is in snapshots, so a snapshot taken once
configured restores a configured device.

```python
board={
    "USB": popcorn.Periph(host=UsbHostOptions()),
},
```

The host's CDC ACM driver binds the CDC function at configuration: SET_LINE_CODING 115200 8N1,
then SET_CONTROL_LINE_STATE with DTR and RTS. `host.cdc()` is the function as a byte stream,
`machine.board.pty("USB")` bridges it to a host pty any serial tool opens.

Python drives the host from the test's coroutine, with the machine parked or inside
`machine.running()`. Both give the same packets for the same input.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32h573/usb_cdc
```

## Read first

- `test_usb_cdc_h573.py`: `machine.periph.USB.host` is the virtual host. `wait_configured` runs the
  machine until the device is configured, `device_descriptor`, `strings` and `interfaces` give what
  the host read, compared with the descriptors in the firmware through `machine.var`, and `control`
  sends a request of the test's own. The controller registers show the address and the endpoint
  numbers the driver set. `host.cdc()` writes and reads the echo, across a snapshot, an unplug and a
  replug, parked and inside `running()`, and through a pty.
- `firmware/usb_descriptors.c`: the device, configuration and string descriptors.
- `firmware/main.c`: the clock and USB power setup and the CDC echo.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH, which also fetches TinyUSB:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32h573/usb_cdc/usb_cdc.elf examples/stm32h573/usb_cdc/prebuilt/
```
