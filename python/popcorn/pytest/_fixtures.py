"""The plugin's `machine_factory` fixture and the bodies of the declared machine and system
fixtures."""

from __future__ import annotations

import contextlib
from collections.abc import AsyncGenerator, Callable, Generator, Iterator
from dataclasses import dataclass

import pytest

from popcorn import (
    Machine,
    MachineDescription,
    MissingInput,
    PopcornError,
    Snapshot,
    System,
    SystemDescription,
)
from popcorn._machine import check_machine_name
from popcorn.pytest._options import UNSET, marker_options
from popcorn.pytest._report import (
    TraceOnFail,
    kept_ends,
    trace_directory,
    track,
    track_system,
    unclaimed,
)
from popcorn.pytest._session import Devices, Key, PopcornSession, Slot, popcorn_session


class _Factory:
    """Machines a fixture made for one test, parked and closed after it."""

    def __init__(self, session: PopcornSession, item: pytest.Item, budget: int | None) -> None:
        self._session = session
        self._item = item
        self._budget = budget
        self._machines: list[tuple[str, Machine]] = []

    def close(self) -> None:
        """Park every machine made, collect its coverage and close its pty bridges.

        What ended a free run by itself and never reached the test is raised, the first one,
        after every machine is closed. The others are warned as unraisable.

        Raises:
            BaseException: What ended the first such free run.
        """
        ends: list[BaseException] = []
        for label, machine in self._machines:
            for ended in kept_ends(self._item, machine):
                ends.append(ended)
                if len(ends) > 1:
                    unclaimed(f"{label} of {self._item.nodeid}", ended)
            self._session.collect(machine)
            machine.board.close()
        self._machines.clear()
        if ends:
            raise ends[0]

    def _ready(self, key: Key, machine: Machine, snapshot: Snapshot, label: str) -> Machine:
        self._keep(machine, key, label)
        machine.restore(snapshot)
        machine.default_budget = self._budget
        return machine

    def _track_failed(self, key: Key, label: str) -> None:
        failed = self._session.failed_machine(key)
        if failed is not None:
            track(self._item, label, failed, key)

    def _keep(self, machine: Machine, key: Key, label: str) -> Machine:
        self._machines.append((label, machine))
        track(self._item, label, machine, key)
        return machine


class MachineFactory(_Factory):
    """Builds machines of any `popcorn.MachineDescription` for one test, each a new one, never a
    declared machine fixture's, parked and closed after the test.

    `factory(description)` gives a new machine restored from the boot snapshot of its key,
    booting the key first when no test did yet. From inside an async test, use
    `await factory.booted(description)` for a key not booted yet: the sync form cannot boot while
    the event loop runs. `factory(description, booted=False)` gives a new machine as
    `MachineDescription.machine` builds it, powered off and not reset, for a test that boots it
    itself. A key boots once per process, shared with the declared machine fixtures of the same
    description, config and strictness.

    Booted machines get the test's default instruction budget, the `budget` of its marker or
    `--popcorn-budget`. Machines not booted get none: it would bound their boot too.

    A machine description with `missing="skip"` skips the test instead of raising MissingInput,
    as a declared machine fixture does.

    `devices` gives bus devices instead of the machine description's, as
    `MachineDescription.machine(devices=)` takes them. A device serves one machine: an instance
    given before, to a machine still open, is refused. A boot with devices is shared, in the
    process and through the boot cache, with the boots whose devices have the same classes, class
    sources, `snapshot()` and instance attributes. Devices holding anything other than plain
    values or their own methods, such as a function, and `popcorn.devices` devices are booted for
    each machine. Not keyed: class attributes changed at run time, module globals or class-level
    containers a device reads, and the modules a device imports. A stale boot matters only when
    the firmware talks to the device before `boot.until`.
    """

    def __init__(self, session: PopcornSession, test: ItemSettings, item: pytest.Item) -> None:
        """Make machines for the test `item` with `test` settings."""
        super().__init__(session, item, test.budget)
        self._strict = test.strict

    def __call__(
        self,
        description: MachineDescription,
        *,
        config: str | None = None,
        strict: bool | None = None,
        booted: bool = True,
        devices: Devices | None = None,
        name: str | None = None,
    ) -> Machine:
        """A new machine of the config `config` of `description`.

        Args:
            description: The `MachineDescription` to build.
            config: The config name, None for the base description.
            strict: Whether unmodelled peripheral accesses raise. None takes the test's marker,
                then `--popcorn-strict`, then the `strict` of the selected config.
            booted: Whether the machine is restored from the boot snapshot of its key, or left
                powered off and not reset.
            devices: Bus devices instead of the machine description's devices of their
                peripherals.
            name: Name of the machine in its log records and errors, the part when None. The
                machine that boots a key for the boot cache is another one and logs its boot
                under the part, unless `devices` are given: then the machine returned is the
                one that booted.

        Raises:
            TypeError: `description` is not a `popcorn.MachineDescription`.
            ValueError: `name` is empty or holds whitespace or a dot.
            RuntimeError: `booted` and the key is not booted yet while an event loop runs.
            ConfigError: The machine description has no config `config`, a device serves another
                live machine, or `MachineDescription.machine` refuses `devices`.
        """
        key = self._key(description, config, strict, name)
        with _skipping_missing(self._session.project(key)):
            if not booted:
                return self._keep(self._session.build(key, devices, name), key, self._label())
            if devices is not None:
                key = self._session.device_key(key, devices)
            try:
                if devices is None:
                    snapshot = self._session.booted(key).decoded()
                    return self._ready(
                        key, self._session.build(key, name=name), snapshot, self._label()
                    )
                return self._ready(
                    key, *self._session.booted_with(key, devices, name), self._label()
                )
            except PopcornError:
                self._track_failed(key, f"machine_factory boot of {key}")
                raise

    async def booted(
        self,
        description: MachineDescription,
        *,
        config: str | None = None,
        strict: bool | None = None,
        devices: Devices | None = None,
        name: str | None = None,
    ) -> Machine:
        """A new booted machine as `factory(description)` gives, booting the key on the running
        loop."""
        key = self._key(description, config, strict, name)
        with _skipping_missing(self._session.project(key)):
            if devices is not None:
                key = self._session.device_key(key, devices)
            try:
                if devices is None:
                    snapshot = (await self._session.booted_async(key)).decoded()
                    return self._ready(
                        key, self._session.build(key, name=name), snapshot, self._label()
                    )
                return self._ready(
                    key, *await self._session.booted_with_async(key, devices, name), self._label()
                )
            except PopcornError:
                self._track_failed(key, f"machine_factory boot of {key}")
                raise

    def _key(
        self, description: object, config: str | None, strict: bool | None, name: str | None
    ) -> Key:
        if not isinstance(description, MachineDescription):
            raise TypeError(
                "machine_factory takes a popcorn.MachineDescription first, not "
                f"{type(description).__name__}"
            )
        check_machine_name(name)
        return self._session.key(description, config, self._strict if strict is None else strict)

    def _label(self) -> str:
        return f"machine_factory #{len(self._machines) + 1}"


class SystemFactory(_Factory):
    """Boots the members of a `popcorn.SystemDescription` for one test, each a new machine restored
    from the boot snapshot of its own machine description's key, parked and closed after the
    test."""

    def __init__(self, session: PopcornSession, item: pytest.Item, fixture: str) -> None:
        """Make members for the system fixture `fixture` of the test `item`, with its marker and
        command-line settings.

        Raises:
            pytest.fail.Exception: The marker is malformed.
        """
        test = item_settings(item, session)
        super().__init__(session, item, test.budget)
        self._strict = test.strict
        self._fixture = fixture

    async def member(self, name: str, project: MachineDescription) -> Machine:
        """A new machine named `name` of the base config of `project`, booted from its key,
        reported as `<fixture> member <name>`."""
        key = self._session.key(project, None, self._strict)
        label = f"{self._fixture} member {name}"
        with _skipping_missing(self._session.project(key)):
            try:
                snapshot = (await self._session.booted_async(key)).decoded()
                return self._ready(key, self._session.build(key, name=name), snapshot, label)
            except PopcornError:
                self._track_failed(key, f"{label} boot")
                raise


_NODE = "node"


@dataclass(frozen=True, slots=True)
class ItemSettings:
    """The machine settings of one test, from its `popcorn` marker over the command line.

    Attributes:
        strict: Whether unmodelled peripheral accesses raise, None for the `strict` of the
            selected config of each description.
        budget: The default instruction budget of its booted machines, None for unbounded.
    """

    strict: bool | None
    budget: int | None


def item_settings(item: pytest.Item, session: PopcornSession) -> ItemSettings:
    """The settings of `item`: marker over command line, per setting.

    `--popcorn-strict` can only turn strictness on, `strict=False` in a marker turns it off.

    Raises:
        pytest.fail.Exception: The marker is malformed.
    """
    options = marker_options(item)
    settings = session.settings
    strict = options.strict
    if strict is None and settings.strict:
        strict = True
    budget = settings.budget if options.budget is UNSET else options.budget
    return ItemSettings(strict, budget)


@pytest.fixture(scope="session")
def _popcorn_session(request: pytest.FixtureRequest) -> PopcornSession:
    return popcorn_session(request.config)


@contextlib.contextmanager
def booting(item: pytest.Item, session: PopcornSession, key: Key, label: str) -> Generator[None]:
    """Skip `item` on missing input of a machine description with `missing="skip"`, and report
    the machine of a failed boot of `key` under `label`."""
    with _skipping_missing(session.project(key)):
        try:
            yield
        except PopcornError:
            failed = session.failed_machine(key)
            if failed is not None:
                track(item, label, failed, key)
            raise


@contextlib.contextmanager
def lending(
    request: pytest.FixtureRequest, session: PopcornSession, test: ItemSettings, key: Key, name: str
) -> Generator[Machine]:
    """The machine of the declared machine fixture `name` for `key`, restored for the test of
    `request`, reported and traced under `name`.

    The machine gets the test's budget and, with `--popcorn-trace-on-fail`, a trace. At exit it
    is parked, its trace kept or deleted, and it is replaced when spoiled, else its hooks and
    call baseline are reset. What ended a free run by itself and never reached the test is
    raised then.

    Raises:
        BaseException: What ended the first such free run, at exit.
    """
    item = item_of(request)
    lent = _Lent(session, key, name)
    with booting(item, session, key, name):
        m = lent.restored()
    track(item, name, m, key)
    m.default_budget = test.budget
    settings = session.settings
    trace = None
    if settings.trace_on_fail:
        trace = TraceOnFail(item, trace_directory(request.config, settings.trace_dir), name)
        if not trace.start(m):
            trace = None
    yield m
    ends = kept_ends(item, m)
    held = ends[0] if ends else None
    for extra in ends[1:]:
        unclaimed(item.nodeid, extra)
    closed = trace is None
    try:
        if trace is not None:
            trace.finish()
            closed = True
    finally:
        if not closed and held is not None:
            unclaimed(item.nodeid, held)
        if not closed or lent.spoiled():
            lent.rebuild()
        else:
            m.remove_hooks()
            m.set_call_baseline(None)
    if held is not None:
        raise held


@dataclass(slots=True)
class _Lent:
    session: PopcornSession
    key: Key
    name: str
    _slot: Slot | None = None

    def restored(self) -> Machine:
        slot, snapshot = self.session.slot(self.key, self.name)
        self._slot = slot
        return _restored(slot.machine, snapshot, self._rebuilt)

    def spoiled(self) -> bool:
        slot = self._slot
        assert slot is not None
        machine = slot.machine
        return len(machine.images) != slot.image_count or machine.board.ptys != slot.ptys

    def rebuild(self) -> None:
        self.session.rebuild_slot(self.key, self.name)

    def _rebuilt(self) -> tuple[Machine, Snapshot]:
        slot = self._slot = self.session.rebuild_slot(self.key, self.name)
        return slot.machine, self.session.booted(self.key).decoded()


@pytest.fixture
def machine_factory(
    request: pytest.FixtureRequest, _popcorn_session: PopcornSession
) -> Iterator[MachineFactory]:
    """A `MachineFactory` for this test, its machines parked and closed after it, as
    `MachineFactory.close` does."""
    item = item_of(request)
    factory = MachineFactory(_popcorn_session, item_settings(item, _popcorn_session), item)
    yield factory
    factory.close()


def item_of(request: pytest.FixtureRequest) -> pytest.Item:
    """The test `request` sets up a fixture for."""
    item: object = getattr(request, _NODE)
    assert isinstance(item, pytest.Item)
    return item


@contextlib.contextmanager
def _skipping_missing(project: MachineDescription) -> Generator[None]:
    try:
        yield
    except MissingInput as error:
        if project.missing != "skip":
            raise
        pytest.skip(f"missing input: {error.path}")


def _restored(
    machine: Machine, snapshot: Snapshot, rebuilt: Callable[[], tuple[Machine, Snapshot]]
) -> Machine:
    assert not machine.busy, "the previous test's teardown parks the machine"
    try:
        machine.restore(snapshot)
    except PopcornError:
        machine, snapshot = rebuilt()
        machine.restore(snapshot)
    machine.ring.clear()
    return machine


@contextlib.asynccontextmanager
async def linking(
    request: pytest.FixtureRequest,
    session: PopcornSession,
    spec: SystemDescription,
    fixture: str,
) -> AsyncGenerator[System]:
    """`spec` booted and linked for the declared system fixture `fixture` of the test of
    `request`, each member a new machine named as the member, reported as
    `<fixture> member <name>` and traced to `<test>.<fixture>.<member>.pctr`.

    At exit a free run left started is halted, the system closed, the members parked and
    closed. What ended a free run by itself and never reached the test is raised then.

    Raises:
        BaseException: What ended the first such free run, at exit.
    """
    item = item_of(request)
    factory = SystemFactory(session, item, fixture)
    settings = session.settings
    traces: list[TraceOnFail] = []
    try:
        machines = {name: await factory.member(name, p) for name, p in spec.members.items()}
        if settings.trace_on_fail:
            directory = trace_directory(request.config, settings.trace_dir)
            for name, m in machines.items():
                trace = TraceOnFail(item, directory, f"{fixture}.{name}")
                if trace.start(m):
                    traces.append(trace)
        linked = spec.link(machines)
    except BaseException:
        _end_traces(traces)
        factory.close()
        raise
    track_system(item, linked)
    yield linked
    held: BaseException | None = None
    try:
        try:
            await linked._teardown()  # pyright: ignore[reportPrivateUsage]
        except Exception as ended:
            held = ended
        finally:
            _end_traces(traces)
    finally:
        try:
            factory.close()
        except BaseException as ended:
            if held is None:
                raise
            unclaimed(item.nodeid, ended)
    if held is not None:
        raise held


def _end_traces(traces: list[TraceOnFail]) -> None:
    errors: list[BaseException] = []
    for trace in traces:
        try:
            trace.finish()
        except Exception as error:
            errors.append(error)
    if errors:
        raise errors[0]
