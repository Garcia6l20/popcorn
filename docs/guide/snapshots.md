# Snapshots

A `popcorn.Snapshot` is the whole state of a machine at one instruction boundary. Take one to
return to a state later, or save it to a file to start another machine from it. The pytest
plugin uses the same mechanism: it restores the boot snapshot before every test.

## Take and restore

```python
--8<-- "guide/test_snapshots.py:restore"
```

`machine.snapshot()` takes the state and `machine.restore(snapshot)` puts it back, any number of
times. The snapshot holds the core registers, the memory, the call arena and the state of every
peripheral model. Both calls raise `MachineRunning` while the machine runs, so take them at a stop.

A machine that is a member of a linked system refuses both with `MachineBusy`. Use
`System.snapshot()` and `System.restore()`, see [Systems and links](systems.md).

## Save to a file

```python
--8<-- "guide/test_snapshots.py:save"
```

`snapshot.save(path)` writes the saved form through a temporary file renamed over the target, so
a reader never sees half a file. `Snapshot.load(path)` and `Snapshot.from_bytes(data)` read it
back, and `machine.load_snapshot(path)` reads and restores in one call. `snapshot.to_bytes()`
gives the same BSON bytes without a file.

A saved snapshot loads into any machine built from the same description, images and
configuration. Another machine, even in another process, continues from the same instruction.

## What a file must match

```python
--8<-- "guide/test_snapshots.py:refused"
```

`restore` raises `ConfigError` for a snapshot another machine object took. Save it and load the
file instead. `load_snapshot` raises it when the file names another description, other images or
another configuration, and the error names the digest that differs.
`snapshot.description_digest`, `image_digests` and `config_digest` show them.

A snapshot restores only into a machine with the same set of peripheral models. One taken by a
popcorn build that lacked a model the current build has is refused. Take it again.

A saved snapshot carries its format version. popcorn reads format 24 and raises `ConfigError` on
the key `format` for a file of any other format. Take the snapshot again with the current build.

A snapshot keeps the analog levels on the pins: the board drive of each GPIO pin and the level a
peripheral output drives on it.

## What a restore leaves out

Host side state that the machine cannot rewind is dropped or kept as follows:

- UART frames the firmware sent and Python did not read yet are dropped. So are bytes waiting
  for a pty.
- Host input not yet delivered to the firmware is kept. Bytes already in the UART receiver
  return to the snapshot's state.
- SAI frames not read yet are dropped, and each `AudioStream.dropped` restarts at 0.
- A USB host returns to the snapshot's state, its cable and queued control requests included.
  Results of control requests not collected yet are dropped.
- A CDC stream drops the bytes the device sent and Python did not read, and gets back the bytes
  written and not yet taken by the device.
- A `PdmMic` drops its queued input and resets its modulator.
- A machine's `VirtualLoop` holds nothing in a snapshot. A restore empties it first: it drops every
  timer and ready callback, cancels every task and runs it to its `finally`, closes the async
  generators left unfinished and clears its exception handler and task factory. A `finally` runs on
  the thread calling `restore` and sees the old timeline. Its error raises as a `ModelError` once
  the restore has finished.
- A `UartDevice` keeps its Python attributes, and its `restore(data)` runs at the restored clock
  with what its `snapshot()` returned. By default that runs `on_attach` again in the loop's event at
  the restored clock, behind every event pending at it. A rerun starts the stack's timers from the
  restored clock, so its stamps equal the first run's only for a snapshot at the attach stamp, and a
  fixture boot gets the stamps of a stack started at `boot.until`.
- The default `snapshot()` raises `SnapshotRefused` once traffic crossed since the last attach:
  `send`, `send_break`, `drive_cts`, `on_frame`, `on_break`, `on_error`, an `on_rts` delivered to a
  class that overrides it, and past the attach stamp a `drive_pin` or an edge delivered to a
  `watch_pin` watch or a rate delivered to a `watch_pin_clock` watch made from the loop. A watch
  made from the test thread counts nothing, and a drive at the attach stamp or from `restore` still
  snapshots. A device with such traffic overrides `snapshot()` and `restore()`. `System.restore`
  takes a snapshot of each member first, so a member whose default stack heard traffic cannot
  restore even a quiet snapshot, where a lone machine can.
  On the fixtures' boot path a refusal is a failed boot: every test of that machine description
  errors with the refusal, which names the device.
  The [UART devices](uart_devices.md#snapshots-and-restores) guide has a stack that overrides both.
- A `UartDevice` with `tapping = True` records the frames of its port as `TapFrame`, both
  directions, and `tap_frames()` takes them. A restore drops the frames the tap holds and restarts
  `index` and `tap_dropped` at 0, a machine reset keeps them. The tap is no traffic: it never makes
  the default `snapshot()` refuse, and taking its frames never lifts a refusal. A machine restored
  from a snapshot at the attach stamp goes on as the run it was saved from, tapped or not, and taps
  the same frames at the same times.
- A `watch_pin` watch stays across a restore, and pin levels change with no edge, so read
  `pin_level` after one. An edge pending in the snapshot is heard in the run after the restore, as
  the run that went on heard it, by the watches standing ahead of a rerun, and both runs refuse the
  next default `snapshot()`. What such an edge arms stays beside the new stack, so a stack that arms
  from the edge and from the level in `on_attach` checks it armed once. A snapshot holds no watch,
  so a new machine has none until a stack makes it. A default `restore` that runs `on_attach` again
  ends the watches the device made from the loop just before it.
- A `watch_pin_clock` watch stays across a reset and a restore. A reset drops the calls pending and
  calls once with the new rate, at the reset time, when it changed the rate. A restore changes the
  rate with no call and brings back the calls its snapshot holds pending, which the standing
  watches hear, so read `pin_clock` after one. A rate delivered from the loop past the attach
  stamp refuses the next default `snapshot()` as an edge does, and a default `restore` that runs
  `on_attach` again ends the watch the device made from the loop just before it.
- A frame's `on_error` pending in a snapshot, a cut the firmware made just before it, replays in the
  run after the restore. A machine reset drops it.

## Hooks across a restore

```python
--8<-- "guide/test_snapshots.py:hooks"
```

Intercepts, return hooks, watches and probes are kept across `restore()`. A probe's count is not
rewound. `machine.remove_hooks()` removes them. See [Hooks and probes](hooks.md).
