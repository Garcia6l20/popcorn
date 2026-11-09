# UART devices

A `popcorn.board.UartDevice` is the far end of a USART, UART or LPUART, written in Python. It hears
each frame the firmware transmits at the frame's virtual time, answers at the baud the firmware
configured, and runs asyncio timers and tasks on a loop that counts the machine's virtual time. The
same code gives the same bytes at the same stamps on every run.

Use it for a peer with its own timing, such as a modem, a radio module or a protocol stack built on
asyncio. A script that only feeds bytes and reads replies from the test uses the host end of the
port instead, see [First machine](first_machine.md).

## A modem on USART2

The peer collects bytes until a line ends and answers 5 ms of virtual time later:

```python
--8<-- "guide/test_uart_devices.py:modem"
```

`on_attach` builds the stack. It runs in the machine's first step, with the loop running, and again
after a restore, see [Snapshots and restores](#snapshots-and-restores).
`on_frame(byte, at)` runs on the machine's thread with the byte and the stamp of the frame's stop
bit. `self.loop.call_later(delay, callback)` arms a timer in virtual time, and `self.send(data)`
queues frames towards the firmware's receiver.

Names starting with `_` that popcorn's `UartDevice` defines are reserved: a subclass defining one,
such as `_reset` or `_frame`, raises `TypeError` at class definition. Other `_` names are free, and
an attribute a subclass sets, such as `self._loop`, never replaces the device's own state. An
instance attribute named like a reserved name, such as `self._reset`, raises `TypeError` when the
machine takes the device. In device code, mark each hook method `@override`, imported with
`from typing import override`, and pyright's `reportImplicitOverride` flags an accidental override
of any base method at check time.

Attach the device through `devices=` of the machine, or `Periph(devices=[Device(cls)])` in a machine
description, alone on its UART:

```python
--8<-- "guide/test_uart_devices.py:start"
```

```python
--8<-- "guide/test_uart_devices.py:run"
```

The firmware is the `uart_mock` example's. Its AT client sends three commands and records how each
was answered.

## Time and stamps

- Every hook receives `at`, counted from the last reset of any kind as `Machine.time` is. Inside a
  hook or a timer `self.now` is the logical stamp: the `at` of the hook or the due time of the
  timer. A reply placed from `now` keeps its exact distance from the frame.
- The machine's clock moves by translated block. A step runs at the first block boundary at or after
  its stamp, so `Machine.time` can read up to one block later than `now`.
- A stamp a device or a timer set is exact at any block shape. A register access stamps at the end of
  its block: `on_error`, a rising `on_rts`, and a frame the firmware started, whose end counts from
  its store.
- At one instruction count, lane then post order decide, the frame hooks ahead of `on_rts` and
  `on_error`, and a hook's `at` is held at the last stamp, so `time()` never goes back.
- A frame keeps its place on the time line. When the dispatch lag passes a frame time, two frames can
  land on one block boundary. A firmware polling slower than the frame rate overruns as on silicon.
  A straight-line block shorter than one frame time keeps the frames apart: that holds up to 160
  instructions with the core and the USART kernel clock equal, and 80 with OVER8.
- `on_error` is a machine event at its stamp. An exception from it stops the next run, not the write.
  A parked write hears it on the machine's thread at the next run.

## Flow control and break

- `rts` reads the firmware's RTS. It is False only with CR3.RTSE set, CR3.DEM clear and the receiver
  full, and True on a UART without flow control. A class that overrides `on_rts(ready, at)` hears
  each edge, one GIL entry per edge. The choice is fixed at construction, so a stack that registers
  its handler later forwards from the override.
- `send(data, obey_rts=True)` holds a frame before its start bit while RTS is False, and starts it no
  earlier than the edge that sets RTS. With `obey_rts=False` the frame starts at its time and
  overruns a full receiver, setting ORE as on silicon. Frames start in queue order.
- A stack that holds RTS False resets it in `reset`, since a machine reset sets the level True with
  no edge.
- `drive_cts(ready, at=)` drives the firmware's nCTS input. With CR3.CTSE a frame in flight
  completes and the next waits for `ready`. The level is board state and survives a machine reset.
- `send_break(at=)` queues a break after the queued frames. It reads as a 0 byte with the framing
  error flag and holds the line one frame time plus 2 bit times. A break from the firmware reaches
  `on_break(at)`, in every line format.

## Line format

`UartDevice(line=LineFormat(baud, parity, stop_bits))` gives the device a format of its own, 8 data
bits. `send` shapes each frame in it, taking the format at the call. The firmware's UART reads the
frame by sampling its waveform, so a frame in another baud, parity or stop length reads as the
sampler gives.

The sampler is the model's: three samples per bit at 7, 8 and 9 sixteenths of the bit time with a
majority vote. It is inferred, no manual states the outcome of a baud mismatch. The device hears
the firmware's frames through the same sampler at its own bit times:

- The format read is the one set when the firmware's stop bit ends. The device-to-firmware direction
  takes the format at `send`.
- A frame read without a framing or parity error reaches `on_frame` with the sampled byte, also when
  only noise was sampled.
- A framing or parity error reaches `on_error` only, as `UartError.FRAMING` or `UartError.PARITY`.
  The stamp is the firmware's stop-bit end.
- A frame in another baud reaches the firmware at the later of its stop-bit end and the UART's last
  stop sample. A faster device has its next frame on the line before RTS can hold it.
- A `line` with the UART's word shape and frame time to a picosecond reads as no `line` does.
- `device.line` is a Python attribute that survives a restore. Set it with the machine parked, from a
  hook or from a loop callback. A running machine refuses a set from the test thread with
  `MachineRunning`, and `loop.post(setattr, device, "line", line, at=)` sets it at a stamp. A frame
  `send` queued keeps its format, and the firmware's next frame is read in the format set when its
  stop bit ends.

A device at 230400 against a firmware at 115200 hears framing errors, and the firmware, hearing
garbage, times out:

```python
--8<-- "guide/test_uart_devices.py:baud"
```

## The virtual loop

`UartDevice.loop` is `Machine.loop`, a `VirtualLoop`. It is a real `asyncio.AbstractEventLoop`
stepped by machine events. `time()` is the virtual time in seconds, `call_later`, `call_at` and
`create_task` work as in asyncio, and `asyncio.sleep` and `asyncio.wait_for` follow virtual time. A
timer fires at the first block boundary at or after its stamp and `time()` inside it is the stamp.

- Hooks, timers and tasks run on the machine's thread, each inside a step. A task made in a step
  takes its first step at the same stamp.
- A task failure nothing retrieved by the end of its step stops the run as a `ModelError`, and
  `set_exception_handler` takes it over. A retrieval made only by another step at the same
  picosecond is too late.
- A repeating timer counts as something due, so give a run with one armed a `for_` or a `deadline`.
- `loop.close()` cancels the tasks and runs them to their `finally`. Close before dropping a machine
  whose stack awaits in a `finally`. The `popcorn.pytest` fixtures close what they drop.
- I/O, executors, signals, `call_soon_threadsafe` and the `run_*` methods raise
  `VirtualLoopUnsupported`.

### Posting from the test

`loop.post(callback, *args, at=)` runs `callback` at the stamp `at` of the machine's time, on the
machine's thread, from the test thread, parked or running. It returns a `Pending` that resolves
with the callback's return value once it ran:

```python
--8<-- "guide/test_uart_devices.py:post"
```

- `at` is required. A running machine has no deterministic "now" and `call_soon_threadsafe` is
  refused for that reason.
- A parked machine raises `ValueError` for a stamp it passed. A running machine that passed the
  stamp when the command arrives fails the result with `ValueError`. Post before `running()`
  starts, or well ahead of the machine, for the same result on every run.
- A `close()` from a callback takes effect when its step ends, so the later posts of that step
  still run.
- A timer due at `at` runs ahead of a post at `at`. Two posts at one stamp run in call order.
- An exception of the callback reaches the result and stops nothing. A cancelled post never runs.
  A reset or a restore before the stamp fails the result with `EmulationError`.
- From the machine's thread, such as a hook, use `call_at`.

### Determinism

The machine decides every stamp, so a stack is deterministic when its own choices are. Seed every
source of randomness the stack draws from and keep wall time out of it. The jittery modem draws its
delay from a generator it seeds in `on_attach`:

```python
--8<-- "guide/test_uart_devices.py:seed"
```

The same seed gives the same reply stamps in every run and every process. A source the stack draws
from unseeded, such as the global generator or `secrets`, gives other bytes in each run.

## Pins

`self.board` is the device's `DeviceBoard`. `watch_pin(pin, on_edge)` calls `on_edge(level, at)`
at every level change of a pin, whoever causes it. `drive_pin(pin, level, at=)` drives a pin from
outside the MCU, `pin_level` and `pin_mode` read it:

```python
--8<-- "guide/test_uart_devices.py:pins"
```

- A firmware store stamps its edge at the end of its block. A pin drive stamps at its exact time.
- A callback runs at the first event boundary, never inside the store. It reads other pins at its
  dispatch, so watch every pin the stack needs and keep the levels from the callbacks.
- A watch stays across resets and restores, which change levels with no edge, so read `pin_level`
  after one.

## A clock the MCU drives on a pin

`self.board.pin_clock(pin)` reads the frequency in Hz the MCU drives onto a pin, 0 when none, and
`watch_pin_clock(pin, on_change)` calls `on_change(hz, at)` at every change of it. The pin is the
low-speed clock output LSCO of `RCC_BDCR`: PA2 on the STM32U5 and the STM32L4, PB2 on the STM32H5.
`LSCOEN` enables it and `LSCOSEL` picks the LSI or the LSE:

```python
--8<-- "guide/test_uart_devices.py:pin-clock"
```

- The LSI runs at the family nominal, 32 kHz on the STM32U5, STM32H5 and STM32L4, or at
  `RccOptions(lsi_hz=)`, set within the datasheet limits of the family.
- The STM32U5 LSE reaches the output only after `LSESYSEN` is set and `LSESYSRDY` follows.
- The rate does not depend on `pin_mode`, inferred: the manuals only say the LSCO pin is left in
  its reset state.
- A write that leaves the rate as it was calls nothing. The call comes at the first event boundary
  at or after the change, with `at` the stamp of the model event that carries the change: a register
  write stamps at the end of its block, a source that was still starting stamps at the first
  instruction boundary at or after its ready instant, 100 us after the write for the LSI.
- A reset drops the calls pending and calls once with the new rate, at the reset time, when it
  changed the rate. A restore changes the rate with no call and brings back the calls its snapshot
  holds pending, so read `pin_clock` after one.

## An SAI block

`self.board.audio_port(sai, block, on_frame)` makes the device the far end of an SAI block when the
firmware is the master. The device hears each frame a transmitting block sends, and the returned
`DeviceAudio` queues the words a receiving block takes:

```python
--8<-- "guide/test_uart_devices.py:codec"
```

```python
--8<-- "guide/test_uart_devices.py:audio"
```

- A block has one owner. An `AudioStream` of the block and a device refuse each other, and so do two
  devices.
- `on_frame(frame)` runs on the machine's thread in the loop at the frame's end stamp. `frame.at` is
  its start. The words and layout equal an `AudioStream` capture of the same run.
- The device hears every layout the block is configured for, such as 4 slots or 32 data bits.
- `DeviceAudio.write(words)` queues one word per slot in slot order. A frame taken with a slot
  empty gets 0 there and counts in `starved`. `queued` counts the words not served yet.
- `write` runs on the machine's thread or from a parked test thread. A running machine raises
  `MachineRunning`, and `loop.post(audio.write, words, at=)` writes at a stamp.
- A frame the device hears, and a `write` outside `on_attach`, are traffic for the default
  `snapshot` of a `UartDevice`, which then raises `SnapshotRefused`. A device with audio overrides
  `snapshot` and `restore`.
- A restore drops the queued words and resets `starved` to 0. The owner and the callback stay, so an
  overriding `restore` writes its words again.
- Each frame costs about 10 us of wall time and one GIL entry, measured on a 33-frame run. At
  16 kHz that is about 160 ms of wall time per virtual second.

## Tap

`tapping = True` records every frame of the port, both directions, as a `TapFrame`.
`tap_frames(towards)` takes them, `"device"` for the frames the firmware sent and the UART's name for
the frames the device sent:

```python
--8<-- "guide/test_uart_devices.py:tap"
```

- The tap shows each frame as it was on the wire, with the start-bit and stop-bit times of its
  sender. A break ends at its last low bit. A frame the firmware aborted is `lost`.
- A tap keeps 1 Mi frames per direction not taken. The oldest are dropped and counted in
  `tap_dropped`.
- The tap is no traffic for a snapshot: it never makes the default `snapshot` refuse.

## Snapshots and restores

A restore rebuilds the stack. Python attributes are never rewound, so the stack keeps its state in
what `on_attach` builds, or in the bytes of `snapshot` that `restore` reads.

- A restore empties the machine's loop, then calls `restore(data)`. The default runs `on_attach`
  again in the loop's event at the restored clock, behind every event pending at it.
- State set in `__init__` survives a restore. `on_attach` must set every attribute the stack
  changes, as the modem does for `buffer` and `commands`, or a rerun starts from the last run's
  values and can answer twice.
- The rerun starts the stack's timers from the restored clock, so its stamps equal the first run's
  only for a snapshot at the attach stamp.
- The default `snapshot` raises `SnapshotRefused` once traffic crossed since the last attach, which
  a rerun cannot rebuild: `send`, `send_break`, `drive_cts`, `on_frame`, `on_break`, `on_error`,
  an `on_rts` delivered to a class that overrides it, a heard audio frame, a `DeviceAudio.write`
  outside `on_attach`, and a `drive_pin` or watched edge past the attach stamp. The error names
  the device.
- `snapshot` and `restore` can run before `on_attach`, so the state they touch exists after
  `__init__`.
- A restore clears the loop's exception handler and task factory, and the machine's context starts
  from its copy made at `Machine()`. A stack that sets them sets them again in `restore`.

```python
--8<-- "guide/test_uart_devices.py:refused"
```

A device whose stack talks, or whose `on_attach` builds no whole stack, overrides `snapshot` and
`restore`. The override keeps what the stack cannot recreate: here the buffer and the replies still
due, each as a stamp:

```python
--8<-- "guide/test_uart_devices.py:resumable"
```

```python
--8<-- "guide/test_uart_devices.py:resume"
```

`restore` runs at the restored clock, so `call_at` re-arms each reply at its own stamp. It sets
every attribute `on_attach` sets, as the default would.

`System.restore` takes a snapshot of each member first, so a member whose default stack heard
traffic cannot restore, even to a quiet snapshot, where a lone machine can. See
[Snapshots](snapshots.md) for what the other models keep.

## Hooks that raise

A device hook that raises stops the run with the exception, once the step has finished. The machine
stays parked and usable, and the next run goes on:

```python
--8<-- "guide/test_uart_devices.py:raising"
```

- `UartDevice`: an exception from `on_attach`, `on_frame`, `on_break`, `on_rts` or `on_error`
  raises from the run as a `ModelError` naming the device. An exception from a timer or a task
  raises as a `ModelError` naming the loop and the callback or task. `restore` raises from the
  machine's `restore`.
- `I2CTarget`: an exception from `on_start`, `on_write`, `on_read`, `on_stop`, `reset` or `restore`
  stops the run once the bus phase has finished for every target. The phase is answered as by an
  absent target: a NACK for `on_start` and `on_write`, a byte of all ones for `on_read`. After a
  raising `on_write` the transfer ends with a NACK and a STOP.
- `SpiTarget`: an exception from `on_exchange`, `on_select`, `reset` or `restore` stops the run
  once the frame has landed. The frame lands as if the device were absent, with all ones on MISO,
  and the transfer goes on. A device that parsed a command byte misses it and answers the rest of
  the transfer out of step.
- `DeviceAudio` callbacks: an exception from `on_frame` stops the run after the frame event. The
  block goes on sending.
- The all-ones answer of an absent or raising I2C or SPI device is the model's open-drain rule, the
  replies ANDed as on an open-drain bus. It is inferred, see [Limits](../support/limits.md).

## Where next

- The [API reference](../reference/python/peripherals.md) lists `UartDevice`, `LineFormat`,
  `Parity`, `UartError`, `DeviceBoard` and `DeviceAudio`. `VirtualLoop` is on the
  [Machines](../reference/python/machines.md) page.
- The C++ side, `uart_endpoint`, is in the [plugin SDK reference](../reference/sdk/index.md).
