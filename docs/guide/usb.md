# USB

popcorn plugs a virtual full-speed USB host into the OTG_FS model. The firmware runs as the
device. The host enumerates it as Linux does, then talks to its CDC, HID and audio functions
with no Python on the transaction path. Everything runs in virtual time.

## Add a host

```python
--8<-- "guide/test_usb.py:describe"
```

`Periph(host=UsbHostOptions())` on OTG_FS gives the model a host with its cable plugged in from
power-on. The host sees the pull-up the firmware enables, waits a 100 ms debounce, resets the
bus and enumerates the device. Both firmwares here use TinyUSB 0.21.0 on OTG_FS in device mode.

`UsbHostOptions` changes the host:

- `UsbHostOptions(attached=False)`: plug the cable later with `await host.attach()`.
- `UsbHostOptions(dtr=False)`: the CDC driver leaves DTR off at configuration.
- `Periph(connect="pty", host=UsbHostOptions())`: bridge the first CDC function to a host pty
  when the machine is built. `host.cdc()` reads then raise `RuntimeError`.

## Enumerate

```python
--8<-- "guide/test_usb.py:enumerate"
```

`machine.periph.OTG_FS.host` is the host. After `wait_configured`, it holds what it learnt: the
`state`, the `address`, the device and configuration descriptors, the `strings` and the
`interfaces` with their endpoints. `host.control(request_type, request, value, index, length)`
sends a control request of your own and raises `UsbStall` when the device stalls it.

A host that is attached and configured stays so across a snapshot. A restore does not
enumerate again.

## Plug and unplug

```python
--8<-- "guide/test_usb.py:detach"
```

`host.detach()` pulls the cable and `host.attach()` plugs it back. The device enumerates again.

## CDC ACM

```python
--8<-- "guide/test_usb.py:cdc"
```

`host.cdc()` is the CDC ACM function as a byte stream. The host's driver sets the line coding
and DTR at configuration. `cdc.bound` turns true at configuration, and `await cdc.wait_ready()`
runs until DTR and RTS went out. `cdc.read_packets()` returns the packets with their times.
`machine.board.pty("OTG_FS")` bridges the function to a pty any serial tool opens, on first call.
The pty path stays the same across a detach and re-attach. As a re-enumerated device is a fresh
port on hardware, the bridge clears `TIOCEXCL` on the pty at the next slice boundary, so a tool that held it exclusively
can open it again.

## HID

```python
--8<-- "guide/test_usb.py:hid"
```

`host.hid(n)` is the n-th HID interface. The host probes it as usbhid does, then polls its
interrupt IN endpoint. `read_reports(n, for_=)` returns the input reports with their times.
`write_report`, `get_feature` and `set_feature` take reports as hidraw does, the report ID first.

## Audio class

```python
--8<-- "guide/test_usb.py:audio"
```

`host.audio()` is a USB Audio 2.0 function. `wait_ready` runs until the host probed it as
snd-usb-audio does. `audio.speaker.write(samples)` sends interleaved integer samples and
`audio.mic.read_frames(n, for_=)` returns 1 ms frames, each with its time. The host's frames
are the device's clock, so there is no drift between the two sides. `speaker.drain()` waits
until every sample left in a packet, `mic.clear()` drops the frames not read yet, and
`speaker.stop()` closes the stream.

The device loses the speaker's first packet after each start, as the example README explains.
Write PCM after the first frames, as the snippet does.

## Specs

The [usb_cdc](examples.md#usb) and [usb_audio](examples.md#usb) examples hold the descriptors,
the firmware and the full test suites. A snapshot restores a USB host to the snapshot's state,
see [Snapshots](snapshots.md).
