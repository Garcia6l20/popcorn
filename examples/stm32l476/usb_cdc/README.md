# USB device enumerated by a virtual host, TinyUSB

An STM32L476 application runs TinyUSB 0.21.0 on the OTG_FS core, RM0351 chapter 47: a CDC ACM
function that echoes what it receives upper-cased, once the host has set DTR. The firmware runs
the core at 48 MHz from MSI range 11 in voltage range 1 with two wait states. The MSI runs in
PLL-mode on the 32.768 kHz LSE, MSIPLLEN, RM0351 6.2.3, and clocks the OTG through
CCIPR.CLK48SEL. It sets PA11 and PA12 to AF10, sets PWR_CR2.USV through
`HAL_PWREx_EnableVddUSB`, enables the OTG clock, calls `tusb_init` and runs `tud_task` in a main
loop that sleeps in WFI. TinyUSB's `dwc2` driver runs the core in device mode with the FIFOs in
slave mode. Descriptors use TinyUSB's example values: VID 0xCafe, PID 0x4001, strings `TinyUSB`
and `TinyUSB Device`.

It ports `stm32l152/usb_cdc`. The L476 has the OTG_FS core of the STM32U575, so the board entry
is `OTG_FS`. The device connects only once firmware sets USV through `HAL_PWREx_EnableVddUSB`,
and popcorn logs one warning that names it while USV is 0.

`Periph(host=UsbHostOptions())` on OTG_FS in the description gives the OTG model a virtual
full-speed host with its cable plugged from power-on. The host sees the pull-up, waits the
100 ms debounce, resets the bus and enumerates the device as Linux does, in virtual time,
without Python on the transaction path. Its state is in snapshots, so a snapshot taken once
configured restores a configured device. The machine runs under `strict=True`.

```python
strict=True,
board={
    "OTG_FS": popcorn.Periph(host=UsbHostOptions()),
},
```

The host's CDC ACM driver binds the CDC function at configuration: SET_LINE_CODING 115200 8N1,
then SET_CONTROL_LINE_STATE with DTR and RTS. `host.cdc()` is the function as a byte stream,
`machine.board.pty("OTG_FS")` bridges it to a host pty any serial tool opens.

## Run

From the repository root:

```sh
uv run pytest -q examples/stm32l476/usb_cdc
```

## Read first

- `test_usb_cdc_l476.py`: `machine.periph.OTG_FS.host` is the virtual host. `wait_configured`
  runs the machine until the device is configured, `device_descriptor`, `strings` and
  `interfaces` give what the host read, compared with the descriptors in the firmware through
  `machine.var`, and `control` sends a request of the test's own. The OTG registers show the
  address and the powered transceiver, PWR_CR2 USV, and RCC the MSI range, LSERDY and MSIPLLEN,
  the CLK48 source and the OTG clock enable. `host.cdc()` writes and reads the echo, across a
  snapshot, an unplug and a replug, parked and inside `running()`, and through a pty.
- `firmware/usb_descriptors.c`: the device, configuration and string descriptors.
- `firmware/main.c`: the clock setup, the USB pins and supply, and the CDC echo.

## Rebuild the firmware

With arm-none-eabi-gcc on PATH, which also fetches TinyUSB:

```sh
uv run pcons -B build -j4
cp build/example-firmware-m4f/examples/stm32l476/usb_cdc/usb_cdc.elf examples/stm32l476/usb_cdc/prebuilt/
```

The build links `../STM32L476XX_FLASH.ld`, 256 KB flash and the 96 KB SRAM1.
