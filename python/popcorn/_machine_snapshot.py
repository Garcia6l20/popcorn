# pyright: reportPrivateUsage=false
"""Snapshot restore bodies behind `Machine.restore`."""

from __future__ import annotations

from typing import TYPE_CHECKING

from popcorn._devices import UartDevice
from popcorn._errors import ConfigError, ModelError, raise_first
from popcorn._snapshot import Snapshot

if TYPE_CHECKING:
    from popcorn._machine import Machine


def restore(machine: Machine, snapshot: Snapshot) -> None:
    machine._restore_done = False
    machine._refuse_member("restore")
    machine._check_snapshot(snapshot)
    raise_first(restore_checked(machine, snapshot))


def restore_checked(machine: Machine, snapshot: Snapshot) -> list[ModelError]:
    """Empty, restore and rebuild, the refusals already run.

    Returns:
        What raised on the way, for the caller to raise once its restore has finished.
    """
    machine._require_idle()
    error = machine._emptied()
    try:
        machine._restore_state(snapshot)
    except BaseException as failed:
        if error is not None:
            failed.add_note(f"then {error}")
        raise
    return ([] if error is None else [error]) + machine._rebuilt()


def check_snapshot(machine: Machine, snapshot: Snapshot) -> None:
    machine._require_idle()
    machine_id = snapshot.machine_id
    if machine_id not in (0, machine._handle.id()):
        raise ConfigError(
            "the snapshot was taken by another machine: restore into the machine that took"
            " it, or load its saved form",
            file="snapshot",
            key="machine",
            expected="restore into the machine that took it, or load its saved form",
        )
    machine._handle.check_restore(snapshot.native)


def emptied(machine: Machine) -> ModelError | None:
    loop = machine._loop
    error = None if loop is None else loop._empty()  # pyright: ignore[reportPrivateUsage]
    machine._model_context.restart()
    return error


def rebuilt(machine: Machine) -> list[ModelError]:
    if machine._loop is None:
        return []
    machine._loop._rewound()  # pyright: ignore[reportPrivateUsage]
    errors: list[ModelError] = []
    for device in uart_devices(machine):
        try:
            machine._model_context.run(device._rebuild)  # pyright: ignore[reportPrivateUsage]
        except ModelError as error:
            errors.append(error)
    return errors


def restore_state(machine: Machine, snapshot: Snapshot) -> None:
    machine._require_idle()
    machine._handle.restore(snapshot.native)
    machine._restore_done = True
    machine._last_stop = None
    machine._ports.discard()
    machine._audio.discard()
    machine._pdm.discard()
    machine._usb.discard()
    if machine._arena is not None:
        machine._arena.reset(snapshot.arena_offset)


def uart_devices(machine: Machine) -> list[UartDevice]:
    return [
        device
        for targets in machine._devices.values()
        for device in targets
        if isinstance(device, UartDevice)
    ]
