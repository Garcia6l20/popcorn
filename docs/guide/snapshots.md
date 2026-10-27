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

A saved snapshot carries its format version. popcorn reads format 8 and raises `ConfigError` on
the key `format` for a file of any other format. Take the snapshot again with the current build.

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

## Hooks across a restore

```python
--8<-- "guide/test_snapshots.py:hooks"
```

Intercepts, return hooks, watches and probes are kept across `restore()`. A probe's count is not
rewound. `machine.remove_hooks()` removes them. See [Hooks and probes](hooks.md).
