"""The plugin's fixtures: `popcorn_project`, `machine` and `machine_factory`."""

from __future__ import annotations

from collections.abc import Iterator

import pytest

from popcorn import Machine, PopcornError, Project
from popcorn.pytest._session import Booted, Key, PopcornSession, popcorn_session


class MachineFactory:
    """Builds machines separate from the `machine` fixture's, closed after the test.

    `factory()` gives a new machine restored from the boot snapshot of its key, booting the key
    first when no test did yet. From inside an async test, use `await factory.booted()` for a
    key not booted yet: the sync form cannot boot while the event loop runs.
    `factory(booted=False)` gives a new machine as `Project.machine` builds it, powered off and
    not reset, for a test that boots it itself.
    """

    def __init__(self, session: PopcornSession, default: Key) -> None:
        """Make machines for one test whose own key is `default`."""
        self._session = session
        self._default = default
        self._machines: list[Machine] = []

    def __call__(
        self, config: str | None = None, *, strict: bool | None = None, booted: bool = True
    ) -> Machine:
        """A new machine of `config`, the test's by default, and `strict`, the config's by
        default.

        Raises:
            RuntimeError: `booted` and the key is not booted yet while an event loop runs.
        """
        key = self._key(config, strict)
        if not booted:
            return self._keep(self._session.build(key))
        return self._restored(key, self._session.booted(key))

    async def booted(self, config: str | None = None, *, strict: bool | None = None) -> Machine:
        """A new booted machine as `factory()` gives, booting the key on the running loop."""
        key = self._key(config, strict)
        return self._restored(key, await self._session.booted_async(key))

    def close(self) -> None:
        """Close the pty bridges of every machine made."""
        for machine in self._machines:
            machine.board.close()
        self._machines.clear()

    def _key(self, config: str | None, strict: bool | None) -> Key:
        config = self._default.config if config is None else config
        if strict is None and config == self._default.config:
            strict = self._default.strict
        return self._session.key(config, strict)

    def _restored(self, key: Key, booted: Booted) -> Machine:
        machine = self._keep(self._session.build(key))
        machine.restore(booted.decoded())
        return machine

    def _keep(self, machine: Machine) -> Machine:
        self._machines.append(machine)
        return machine


def key_of(request: pytest.FixtureRequest, session: PopcornSession) -> Key:
    """The key of the test `request` belongs to."""
    return session.key(None, None)


@pytest.fixture(scope="session")
def _popcorn_session(request: pytest.FixtureRequest) -> PopcornSession:
    return popcorn_session(request.config)


@pytest.fixture(scope="session")
def popcorn_project(_popcorn_session: PopcornSession) -> Project:
    """The project file, `--popcorn-project` or `<rootdir>/popcorn.toml`."""
    return _popcorn_session.project()


@pytest.fixture
def machine(request: pytest.FixtureRequest, _popcorn_session: PopcornSession) -> Iterator[Machine]:
    """The booted machine of this test's key, restored from its boot snapshot.

    Every test of one key gets the same machine object. Hooks, intercept logs, the call baseline
    and the trace ring are reset for each test. Host input queued on a UART and not delivered
    yet is not.
    """
    key = key_of(request, _popcorn_session)
    booted = _restored(_popcorn_session, key)
    m = booted.machine
    yield m
    if _spoiled(m, booted):
        _popcorn_session.rebuild(key)
        return
    m.remove_hooks()
    m.set_call_baseline(None)


@pytest.fixture
def machine_factory(
    request: pytest.FixtureRequest, _popcorn_session: PopcornSession
) -> Iterator[MachineFactory]:
    """A `MachineFactory` for this test, its machines closed after it."""
    factory = MachineFactory(_popcorn_session, key_of(request, _popcorn_session))
    yield factory
    factory.close()


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
