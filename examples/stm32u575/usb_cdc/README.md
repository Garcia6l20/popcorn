# USB device enumerated by a virtual host, TinyUSB

An STM32U575 application runs TinyUSB 0.21.0 on OTG_FS in device mode, slave FIFO mode: a
composite device with a CDC ACM function and two HID interfaces. The CDC function echoes what
it receives upper-cased, once the host has set DTR. The first HID interface sends one 2-byte
input report, report ID 1, each time PC13 rises, and takes output report 2 into `hid_led`
through SET_REPORT, having no interrupt OUT endpoint. The second has an interrupt OUT endpoint
and unnumbered reports: it answers each 8-byte output report with its bytes inverted, and keeps
a 4-byte feature report. The firmware starts HSI48 as the 48 MHz ICLK, powers
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
# connect = "pty"             # bridge the first CDC ACM function to a host pty at build,
#                             # host.cdc() reads then raise RuntimeError
```

The host's CDC ACM driver binds the CDC function at configuration: SET_LINE_CODING 115200 8N1,
then SET_CONTROL_LINE_STATE with DTR and RTS. `host.cdc()` is the function as a byte stream,
`machine.board.pty("OTG_FS")` bridges it to a host pty any serial tool opens.

`cdc.bound` turns True at configuration, before those two requests went out.
`await cdc.wait_ready(for_=...)` runs until they ended, so `cdc.dtr` holds what the device
acknowledged and written bytes go out. With `host = { dtr = false }` the control line state
still goes out, DTR off, and `wait_ready` waits for it the same way.

The pty owns what the device sends. With `connect = "pty"`, every machine of the project is
bridged at build, and `host.cdc()` reads raise
`RuntimeError: OTG_FS is bridged to /dev/pts/N` for the life of the machine. Writes are not
refused, they mix with the pty client's bytes. To keep `host.cdc()` reads in most tests, leave
`connect` out and call `machine.board.pty("OTG_FS")` in the tests that want the pty: it
bridges on first call.

The host's HID driver probes each HID interface as Linux usbhid does, SET_IDLE 0 then
GET_DESCRIPTOR of the report descriptor, and then polls its interrupt IN endpoint.
`host.hid(n)` is the n-th HID interface: `read_reports` gives input reports with their times,
`write_report`, `get_feature` and `set_feature` take reports as hidraw does, report ID first.
`await hid.wait_ready(for_=...)` runs until the report descriptor is read and polling started.

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
  the echo, across a snapshot, an unplug and a replug, and through a pty. `host.hid()` reads
  the report of a PC13 press, `host.hid(1)` the inverted answer to an output report.
- `firmware/usb_descriptors.c`: the device, configuration, HID report and string descriptors.
- `firmware/main.c`: the clock and USB power setup, the CDC echo and the HID reports.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH, which also fetches TinyUSB:

```sh
uv run pcons -B build -j4
cp build/example-firmware/examples/stm32u575/usb_cdc/usb_cdc.elf examples/stm32u575/usb_cdc/prebuilt/
```
