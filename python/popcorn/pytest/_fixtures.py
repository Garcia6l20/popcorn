"""The plugin's fixtures: `popcorn_project`, `popcorn_selected_project`, `machine`,
`machine_factory`, `popcorn_system` and `system`."""

from __future__ import annotations

import contextlib
from collections.abc import AsyncIterator, Generator, Iterator
from dataclasses import dataclass

import pytest

from popcorn import (
    LinkedSystem,
    Machine,
    MachineDescription,
    MissingInput,
    PopcornError,
    Snapshot,
    System,
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
from popcorn.pytest._session import Booted, Devices, Key, PopcornSession, popcorn_session


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
    """Builds machines separate from the `machine` fixture's, parked and closed after the test.

    `factory()` gives a new machine restored from the boot snapshot of its key, booting the key
    first when no test did yet. From inside an async test, use `await factory.booted()` for a
    key not booted yet: the sync form cannot boot while the event loop runs.
    `factory(booted=False)` gives a new machine as `MachineDescription.machine` builds it, powered
    off and
    not reset, for a test that boots it itself. `factory(description=other)` builds from another
    `popcorn.MachineDescription` than the test's, booted once per key as the test's own.

    Booted machines get the test's default instruction budget, the `budget` of its marker or
    `--popcorn-budget`. Machines not booted get none: it would bound their boot too.

    A machine description with `missing="skip"` skips the test instead of raising MissingInput, as
    the `machine` fixture does.

    `devices` gives bus devices instead of the machine description's, as
    `MachineDescription.machine(devices=)` takes them. A device serves one machine: an instance
    given before, to a machine still open, is refused. A boot with devices is shared, in the process
    and through the boot cache, with the boots whose devices have the same classes, class sources,
    `snapshot()` and instance attributes. Devices holding anything other than plain values or their
    own methods, such as a function, and `popcorn.devices` devices are booted for each machine.
    Not keyed: class attributes changed at run time, module globals or class-level containers
    a device reads, and the modules a device imports. A stale boot matters only when the
    firmware talks to the device before `boot.until`.
    """

    def __init__(self, session: PopcornSession, test: ItemSettings, item: pytest.Item) -> None:
        """Make machines for the test `item` with `test` settings."""
        super().__init__(session, item, test.budget)
        self._project = test.project
        self._default = test.key

    def __call__(
        self,
        config: str | None = None,
        *,
        description: MachineDescription | None = None,
        strict: bool | None = None,
        booted: bool = True,
        devices: Devices | None = None,
        name: str | None = None,
    ) -> Machine:
        """A new machine of the config `config` of `project`.

        Args:
            config: The config name, by default the test's for the test's machine description and
                the base for another `description`.
            description: The `MachineDescription` to build, the test's `popcorn_project` by default.
            strict: Whether unmodelled peripheral accesses raise, by default the test's for its
                own machine description and config, else the selected config's `strict`.
            booted: Whether the machine is restored from the boot snapshot of its key, or left
                powered off and not reset.
            devices: Bus devices instead of the machine description's devices of their peripherals.
            name: Name of the machine in its log records and errors, the part when None. The
                machine that boots a key for the boot cache is another one and logs its boot
                under the part, unless `devices` are given: then the machine returned is the
                one that booted.

        Raises:
            ValueError: `name` is empty or holds whitespace or a dot.
            RuntimeError: `booted` and the key is not booted yet while an event loop runs.
            ConfigError: The machine description has no config `config`, a device serves another
                live machine, or `MachineDescription.machine` refuses `devices`.
        """
        check_machine_name(name)
        key = self._key(config, description, strict)
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
        config: str | None = None,
        *,
        description: MachineDescription | None = None,
        strict: bool | None = None,
        devices: Devices | None = None,
        name: str | None = None,
    ) -> Machine:
        """A new booted machine as `factory()` gives, booting the key on the running loop."""
        check_machine_name(name)
        key = self._key(config, description, strict)
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
        self, config: str | None, description: MachineDescription | None, strict: bool | None
    ) -> Key:
        if description is not None:
            return self._session.key(description, config, strict)
        config = self._default.config if config is None else config
        if strict is None and config == self._default.config:
            strict = self._default.strict
        return self._session.key(self._project, config, strict)

    def _label(self) -> str:
        return f"machine_factory #{len(self._machines) + 1}"


class SystemFactory(_Factory):
    """Boots the members of a `popcorn.System` for one test, each a new machine restored
    from the boot snapshot of its own machine description's key, parked and closed after the
    test."""

    def __init__(self, session: PopcornSession, item: pytest.Item) -> None:
        """Make members for the system test `item`, with its marker and command-line settings.

        Raises:
            pytest.fail.Exception: The marker is malformed or names a config.
        """
        options = marker_options(item)
        if options.config is not UNSET:
            pytest.fail(
                "@pytest.mark.popcorn config does not apply to the system fixture: a member's "
                "config is part of its machine description, "
                "System(members={name: project.config(...)})",
                pytrace=False,
            )
        settings = session.settings
        super().__init__(
            session, item, settings.budget if options.budget is UNSET else options.budget
        )
        self._strict = True if options.strict is None and settings.strict else options.strict

    async def member(self, name: str, project: MachineDescription) -> Machine:
        """A new machine named `name` of the base config of `project`, booted from its key."""
        key = self._session.key(project, None, self._strict)
        label = f"system member {name}"
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
    """The machine settings of one test.

    Attributes:
        project: Its `popcorn_project`, no config applied.
        key: The machine description, config and strictness of its machines.
        budget: The default instruction budget of its booted machines, None for unbounded.
    """

    project: MachineDescription
    key: Key
    budget: int | None


def settings_of(
    item: pytest.Item, session: PopcornSession, project: MachineDescription
) -> ItemSettings:
    """The settings of `item` with the machine description `project`: marker over command line over
    machine description, per setting.

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
    return ItemSettings(project, session.key(project, config, strict), budget)


@pytest.fixture(scope="session")
def _popcorn_session(request: pytest.FixtureRequest) -> PopcornSession:
    return popcorn_session(request.config)


MISSING_PROJECT = (
    "define a `popcorn_project` fixture returning a `popcorn.MachineDescription` in a conftest.py "
    "above "
    "this test"
)


@pytest.fixture(scope="session")
def popcorn_project() -> MachineDescription:
    """The machine description of the tests below a conftest.py, overridden there by the user.

    The nearest override wins, as for any pytest fixture. Without one, the tests using
    `machine` or `machine_factory` fail, naming the fix.
    """
    pytest.fail(MISSING_PROJECT, pytrace=False)


@pytest.fixture
def popcorn_selected_project(
    request: pytest.FixtureRequest, _popcorn_session: PopcornSession, popcorn_project: object
) -> MachineDescription:
    """The `popcorn_project` with this test's config applied.

    The config is the one `machine` and `machine_factory` run under: the `popcorn` marker's,
    else `--popcorn-config`, else none, which gives the base machine description. Use it to boot
    machines by hand with the same behaviour as `machine`.

    Raises:
        ConfigError: The selected config does not exist.
    """
    project = _checked(popcorn_project)
    return _popcorn_session.project(settings_of(_item(request), _popcorn_session, project).key)


@pytest.fixture
def machine(
    request: pytest.FixtureRequest, _popcorn_session: PopcornSession, popcorn_project: object
) -> Iterator[Machine]:
    """The booted machine of this test's key, restored from its boot snapshot.

    Every test of one key gets the same machine object. Hooks, intercept logs, the call baseline
    and the trace ring are reset for each test. Host input queued on a UART and not delivered
    yet is not. Its `default_budget` is the test's, from the `popcorn` marker or
    `--popcorn-budget`.

    Teardown parks the machine: a free run the test left started is halted and the machine is
    reused. What ended that free run by itself and never reached the test fails the teardown.

    A machine description with `missing="skip"` skips the test when it raises MissingInput, the
    missing path as the reason, for every test of its key.
    """
    item = _item(request)
    project = _checked(popcorn_project)
    test = settings_of(item, _popcorn_session, project)
    with _skipping_missing(_popcorn_session.project(test.key)):
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
        if not closed or _spoiled(m, booted):
            _popcorn_session.rebuild(test.key)
        else:
            m.remove_hooks()
            m.set_call_baseline(None)
    if held is not None:
        raise held


@pytest.fixture
def machine_factory(
    request: pytest.FixtureRequest, _popcorn_session: PopcornSession, popcorn_project: object
) -> Iterator[MachineFactory]:
    """A `MachineFactory` for this test, its machines parked and closed after it, as
    `MachineFactory.close` does."""
    item = _item(request)
    project = _checked(popcorn_project)
    factory = MachineFactory(_popcorn_session, settings_of(item, _popcorn_session, project), item)
    yield factory
    factory.close()


def _checked(project: object) -> MachineDescription:
    if not isinstance(project, MachineDescription):
        pytest.fail(
            f"popcorn_project returned {type(project).__name__}, not a popcorn.MachineDescription: "
            f"{MISSING_PROJECT}",
            pytrace=False,
        )
    return project


def _item(request: pytest.FixtureRequest) -> pytest.Item:
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


def _restored(session: PopcornSession, key: Key) -> Booted:
    booted = session.booted(key)
    assert not booted.machine.busy, "the previous test's teardown parks the machine"
    try:
        booted.machine.restore(booted.snapshot)
    except PopcornError:
        booted = session.rebuild(key)
        booted.machine.restore(booted.snapshot)
    booted.machine.ring.clear()
    return booted


def _spoiled(machine: Machine, booted: Booted) -> bool:
    return len(machine.images) != booted.image_count or machine.board.ptys != booted.ptys


MISSING_SYSTEM = (
    "define a `popcorn_system` fixture returning a `popcorn.System` in a conftest.py above this "
    "test"
)


@pytest.fixture(scope="session")
def popcorn_system() -> System:
    """The linked system of the tests below a conftest.py, overridden there by the user.

    The nearest override wins, as for any pytest fixture. Without one, the tests using `system`
    fail, naming the fix.
    """
    pytest.fail(MISSING_SYSTEM, pytrace=False)


@pytest.fixture
async def system(
    request: pytest.FixtureRequest, _popcorn_session: PopcornSession, popcorn_system: object
) -> AsyncIterator[LinkedSystem]:
    """The `popcorn_system` booted and linked for this test.

    Each member is a new machine named as the member, restored from the boot snapshot of its
    own machine description's key, booted once per key and process as for `machine`: two members of
    one machine description are two machines of one boot. Never the `machine` fixture's object. Each
    stands at its boot stop, its system time from `System.offsets`.

    Per member, from the `popcorn` marker over the command line over the member's machine
    description: `strict`, and `budget` as its `default_budget`, which bounds its run, wait_for and
    call goals.
    `system.run` is bounded by its `for_` or `until` only. A marker naming a `config` fails the
    test, `--popcorn-config` does not apply to members. With `--popcorn-trace-on-fail`, each
    member is traced to `<test>.<member>.pctr`. With `--popcorn-cov`, each member counts.

    Teardown halts a free run the test left started, closes the system, then parks and closes
    the members. What ended a free run by itself and never reached the test fails the teardown.
    """
    item = _item(request)
    spec = _checked_system(popcorn_system)
    factory = SystemFactory(_popcorn_session, item)
    settings = _popcorn_session.settings
    traces: list[TraceOnFail] = []
    try:
        machines = {name: await factory.member(name, p) for name, p in spec.members.items()}
        if settings.trace_on_fail:
            directory = trace_directory(request.config, settings.trace_dir)
            for name, m in machines.items():
                trace = TraceOnFail(item, directory, name)
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


def _checked_system(spec: object) -> System:
    if not isinstance(spec, System):
        pytest.fail(
            f"popcorn_system returned {type(spec).__name__}, not a popcorn.System: "
            f"{MISSING_SYSTEM}",
            pytrace=False,
        )
    return spec
