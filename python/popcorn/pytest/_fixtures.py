"""The plugin's fixtures: `popcorn_project`, `machine` and `machine_factory`."""

from __future__ import annotations

from collections.abc import Iterator
from dataclasses import dataclass

import pytest

from popcorn import Machine, PopcornError, Project, Snapshot
from popcorn.pytest._options import UNSET, marker_options
from popcorn.pytest._report import TraceOnFail, trace_directory, track
from popcorn.pytest._session import Booted, Devices, Key, PopcornSession, popcorn_session


class MachineFactory:
    """Builds machines separate from the `machine` fixture's, closed after the test.

    `factory()` gives a new machine restored from the boot snapshot of its key, booting the key
    first when no test did yet. From inside an async test, use `await factory.booted()` for a
    key not booted yet: the sync form cannot boot while the event loop runs.
    `factory(booted=False)` gives a new machine as `Project.machine` builds it, powered off and
    not reset, for a test that boots it itself.

    Booted machines get the test's default instruction budget, the `budget` of its marker or
    `--popcorn-budget`. Machines not booted get none: it would bound their boot too.

    `devices` gives bus devices instead of the project file's, as `Project.machine(devices=)`
    takes them. A device serves one machine: an instance given before, to a machine still
    open, is refused. A boot with devices is shared, in the process and through the boot
    cache, with the boots whose devices have the same classes, class sources, `snapshot()` and
    instance attributes. Devices holding anything other than plain values or their own
    methods, such as a function, and `popcorn.devices` devices are booted for each machine.
    Not keyed: class attributes changed at run time, module globals or class-level containers
    a device reads, and the modules a device imports. A stale boot matters only when the
    firmware talks to the device before `[boot] until`.
    """

    def __init__(self, session: PopcornSession, test: ItemSettings, item: pytest.Item) -> None:
        """Make machines for the test `item` with `test` settings."""
        self._session = session
        self._item = item
        self._default = test.key
        self._budget = test.budget
        self._machines: list[Machine] = []

    def __call__(
        self,
        config: str | None = None,
        *,
        strict: bool | None = None,
        booted: bool = True,
        devices: Devices | None = None,
    ) -> Machine:
        """A new machine of `config`, the test's by default, and `strict`, the config's by
        default, with `devices` instead of the project file's devices of their peripherals.

        Raises:
            RuntimeError: `booted` and the key is not booted yet while an event loop runs.
            ConfigError: A device serves another live machine, or `Project.machine` refuses
                `devices`.
        """
        key = self._key(config, strict)
        if not booted:
            return self._keep(self._session.build(key, devices), key)
        if devices is not None:
            key = self._session.device_key(key, devices)
        try:
            if devices is None:
                snapshot = self._session.booted(key).decoded()
                return self._ready(key, self._session.build(key), snapshot)
            return self._ready(key, *self._session.booted_with(key, devices))
        except PopcornError:
            self._track_failed(key)
            raise

    async def booted(
        self,
        config: str | None = None,
        *,
        strict: bool | None = None,
        devices: Devices | None = None,
    ) -> Machine:
        """A new booted machine as `factory()` gives, booting the key on the running loop."""
        key = self._key(config, strict)
        if devices is not None:
            key = self._session.device_key(key, devices)
        try:
            if devices is None:
                snapshot = (await self._session.booted_async(key)).decoded()
                return self._ready(key, self._session.build(key), snapshot)
            return self._ready(key, *await self._session.booted_with_async(key, devices))
        except PopcornError:
            self._track_failed(key)
            raise

    def close(self) -> None:
        """Collect the coverage of every machine made and close its pty bridges."""
        for machine in self._machines:
            self._session.collect(machine)
            machine.board.close()
        self._machines.clear()

    def _key(self, config: str | None, strict: bool | None) -> Key:
        config = self._default.config if config is None else config
        if strict is None and config == self._default.config:
            strict = self._default.strict
        return self._session.key(config, strict)

    def _ready(self, key: Key, machine: Machine, snapshot: Snapshot) -> Machine:
        self._keep(machine, key)
        machine.restore(snapshot)
        machine.default_budget = self._budget
        return machine

    def _track_failed(self, key: Key) -> None:
        failed = self._session.failed_machine(key)
        if failed is not None:
            track(self._item, f"machine_factory boot of {key}", failed, key)

    def _keep(self, machine: Machine, key: Key) -> Machine:
        self._machines.append(machine)
        track(self._item, f"machine_factory #{len(self._machines)}", machine, key)
        return machine


_NODE = "node"


@dataclass(frozen=True, slots=True)
class ItemSettings:
    """The machine settings of one test.

    Attributes:
        key: The config and strictness of its machines.
        budget: The default instruction budget of its booted machines, None for unbounded.
    """

    key: Key
    budget: int | None


def settings_of(item: pytest.Item, session: PopcornSession) -> ItemSettings:
    """The settings of `item`: marker over command line over project file, per setting.

    `--popcorn-strict` can only turn strictness on, `strict=False` in a marker turns it off.

    Raises:
        pytest.fail.Exception: The marker is malformed.
        ConfigError: The config does not exist.
    """
    options = marker_options(item)
    settings = session.settings
    config = settings.config if options.config is UNSET else options.config
    strict = options.strict
    if strict is None and settings.strict:
        strict = True
    budget = settings.budget if options.budget is UNSET else options.budget
    return ItemSettings(session.key(config, strict), budget)


@pytest.fixture(scope="session")
def _popcorn_session(request: pytest.FixtureRequest) -> PopcornSession:
    return popcorn_session(request.config)


@pytest.fixture(scope="session")
def popcorn_project(_popcorn_session: PopcornSession) -> Project:
    """The project file, `--popcorn-project` or `<rootdir>/popcorn.toml`, with `--popcorn-config`
    applied."""
    return _popcorn_session.project(_popcorn_session.settings.config)


@pytest.fixture
def machine(request: pytest.FixtureRequest, _popcorn_session: PopcornSession) -> Iterator[Machine]:
    """The booted machine of this test's key, restored from its boot snapshot.

    Every test of one key gets the same machine object. Hooks, intercept logs, the call baseline
    and the trace ring are reset for each test. Host input queued on a UART and not delivered
    yet is not. Its `default_budget` is the test's, from the `popcorn` marker or
    `--popcorn-budget`.
    """
    item = _item(request)
    test = settings_of(item, _popcorn_session)
    try:
        booted = _restored(_popcorn_session, test.key)
    except PopcornError:
        failed = _popcorn_session.failed_machine(test.key)
        if failed is not None:
            track(item, "machine", failed, test.key)
        raise
    track(item, "machine", booted.machine, test.key)
    m = booted.machine
    m.default_budget = test.budget
    settings = _popcorn_session.settings
    trace = None
    if settings.trace_on_fail:
        trace = TraceOnFail(item, trace_directory(request.config, settings.trace_dir))
        if not trace.start(m):
            trace = None
    yield m
    closed = trace is None
    try:
        if trace is not None:
            trace.finish()
            closed = True
    finally:
        if not closed or _spoiled(m, booted):
            _popcorn_session.rebuild(test.key)
        else:
            m.remove_hooks()
            m.set_call_baseline(None)


@pytest.fixture
def machine_factory(
    request: pytest.FixtureRequest, _popcorn_session: PopcornSession
) -> Iterator[MachineFactory]:
    """A `MachineFactory` for this test, its machines closed after it."""
    item = _item(request)
    factory = MachineFactory(_popcorn_session, settings_of(item, _popcorn_session), item)
    yield factory
    factory.close()


def _item(request: pytest.FixtureRequest) -> pytest.Item:
    item: object = getattr(request, _NODE)
    assert isinstance(item, pytest.Item)
    return item


def _restored(session: PopcornSession, key: Key) -> Booted:
    booted = session.booted(key)
    try:
        booted.machine.restore(booted.snapshot)
    except PopcornError:
        booted = session.rebuild(key)
        booted.machine.restore(booted.snapshot)
    booted.machine.ring.clear()
    return booted


def _spoiled(machine: Machine, booted: Booted) -> bool:
    return (
        machine.busy
        or len(machine.images) != booted.image_count
        or machine.board.ptys != booted.ptys
    )
