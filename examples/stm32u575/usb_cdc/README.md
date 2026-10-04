# USB device enumerated by a virtual host, TinyUSB

An STM32U575 application runs TinyUSB 0.21.0 on OTG_FS in device mode, slave FIFO mode: a
composite device with a CDC ACM function and a HID function. The CDC function echoes what it
receives upper-cased, once the host has set DTR. The HID function sends one 2-byte input
report, report ID 1, each time PC13 rises. The firmware starts HSI48 as the 48 MHz ICLK, powers
VDDUSB, calls `tusb_init` and runs `tud_task` in a main loop that sleeps in WFI. Descriptors
use TinyUSB's example values: VID 0xCafe, PID 0x4005, strings `TinyUSB` and `TinyUSB Device`.

`[board.OTG_FS] host = {}` in `popcorn.toml` gives the OTG_FS model a virtual full-speed host
with its cable plugged from power-on. The host sees the pull-up, waits the 100 ms debounce,
resets the bus and enumerates the device as Linux does, in virtual time, without Python on the
transaction path. Its state is in snapshots, so a snapshot taken once configured restores a
configured device.

```toml
[board.OTG_FS]
host = {}                     # cable plugged at power-on, default timings
# host = { attached = false } # plugged later with `await host.attach()`
# host = { dtr = false }      # the CDC driver leaves DTR off at configuration
# connect = "pty"             # bridge the first CDC ACM function to a host pty
```

The host's CDC ACM driver binds the CDC function at configuration: SET_LINE_CODING 115200 8N1,
then SET_CONTROL_LINE_STATE with DTR and RTS. `host.cdc()` is the function as a byte stream,
`machine.board.pty("OTG_FS")` bridges it to a host pty any serial tool opens.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32u575/usb_cdc
```

## Read first

- `test_usb_cdc.py`: `machine.periph.OTG_FS.host` is the virtual host. `wait_configured` runs
  the machine until the device is configured, `device_descriptor`, `strings` and `interfaces`
  give what the host read, compared with the descriptors in the firmware through
  `machine.var`, and `control` sends a request of the test's own. `host.cdc()` writes and reads
  the echo, across a snapshot, an unplug and a replug, and through a pty.
- `firmware/usb_descriptors.c`: the device, configuration, HID report and string descriptors.
- `firmware/main.c`: the clock and USB power setup, the CDC echo and the HID report.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH, which also fetches TinyUSB:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/usb_cdc/usb_cdc.elf examples/stm32u575/usb_cdc/prebuilt/
```
