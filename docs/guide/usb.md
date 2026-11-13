# USB

popcorn plugs a virtual full-speed USB host into the OTG_FS model. The firmware runs as the
device. The host enumerates it as Linux does, then talks to its CDC, HID and audio functions
with no Python on the transaction path. Everything runs in virtual time.

## Add a host

```python
--8<-- "guide/test_usb.py:describe"
```

`Periph(host=UsbHostOptions())` on OTG_FS gives the model a host with its cable plugged in from
power-on. The host sees the pull-up the firmware enables, waits a 100 ms debounce, resets the bus
for 10 ms and enumerates the device. After each bus reset it sends only SOFs for the 10 ms reset
recovery of USB 2.0 7.1.7.5, then its next request. Both firmwares here use TinyUSB 0.21.0 on OTG_FS
in device mode.

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
The pty path stays the same across a detach and re-attach. As on a real `ttyACM`, a tool that
set `TIOCEXCL` on the pty frees it by closing it: once the last tool closed the pty, the next
`open()` succeeds. A re-enumerated device is a fresh port, so a detach or attach clears the flag
too, also while a tool still holds the pty open. The bridge clears the flag at the next slice
boundary. A close or a re-enumeration the firmware drives happens inside a run and is cleared
before the machine parks. A close, `detach()` or `attach()` on a parked machine is cleared on the
next run, so run the machine before the reopen. The bridge finds the tools that still hold the
pty by scanning `/proc/*/fd`, as described below for a CDC pty, and once per close of an
exclusive UART pty. A tool of another user, another pid namespace, a `hidepid` mount or a
non-dumpable process is invisible to the scan and does not keep the flag. A child forked with the
pty open holds it as on a real port, so the flag stays until the child closes it or exits. If the
kernel refuses the inotify watch the bridge needs, `machine.board.pty()` still works, logs one
warning and clears the flag only at a detach or attach. UART ptys behave the same way.

A CDC pty drives DTR like a `ttyACM` port. The first open of the pty by a tool sends
SET_CONTROL_LINE_STATE with DTR and RTS set, unless the device already has both set. The close
that leaves no tool holding the pty sends it with both clear, so the firmware sees a falling DTR
edge between two tool runs and a rising one at the next. The close drops the lines whatever the
tool's `HUPCL` setting. The bridge sees opens and closes at the slice boundary, like the byte
bridging. A tool that opens and closes the pty inside one slice makes both edges. A close
followed by a reopen inside one slice that leaves a tool holding the pty makes none. A child
forked with the pty open and a second tool keep DTR set until the last of them closed it.

While a tool holds the pty, DTR and RTS are raised again once a configured driver exists after
an open before enumeration, a `restore()` or a `detach()` and `attach()` that left them clear.
A close with no tool holding the pty sends nothing while no driver is configured. The bridge
walks `/proc` once for the tools holding the pty and re-checks the ones it found at the next
closes, so a close costs a walk only when none of them is left. Without the inotify watch the
line state stays as the host set it at configuration. A UART pty and a `CdcLink` used without a
pty keep the line state the host set.

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

The host has no feedback endpoint model and no clock besides the frame, so the isochronous rate
is the host's frame rate: 1 ms per frame, 16 samples a frame at 16 kHz. Every frame carries
exactly the rate's samples. A firmware feedback or adaptive clocking error, and the FIFO
underruns or overruns a drifting clock causes, show only on hardware.

The device loses the speaker's first packet after each start, as the example README explains.
Write PCM after the first frames, as the snippet does.

## Specs

The [usb_cdc](examples.md#usb) and [usb_audio](examples.md#usb) examples hold the descriptors,
the firmware and the full test suites. A snapshot restores a USB host to the snapshot's state,
see [Snapshots](snapshots.md).
