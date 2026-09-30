"""Per-process plugin state: the project, one booted machine per key, the boot counters."""

from __future__ import annotations

import asyncio
import contextvars
import copy
from dataclasses import dataclass, field

import pytest

from popcorn import Machine, Project, Snapshot, Stop
from popcorn.pytest._loop import OwnedLoop, owned_loop
from popcorn.pytest._options import SETTINGS, Settings


@dataclass(frozen=True, slots=True)
class Key:
    """What a booted machine depends on besides the project file.

    Attributes:
        config: The `[configs.NAME]` applied, None for the base file.
        strict: Whether unmodelled peripheral accesses raise.
    """

    config: str | None
    strict: bool

    def __str__(self) -> str:
        return f"config={self.config or '-'} strict={self.strict}"


@dataclass(slots=True)
class Booted:
    """The booted state of one key.

    Attributes:
        machine: The machine the `machine` fixture restores for every test of the key.
        snapshot: The boot snapshot, taken on `machine`.
        saved: `snapshot` in its saved form, restorable into any machine of the key.
        boot_stop: The stop of the boot, None for a project without `[boot] until`.
        image_count: How many images the machine had after the build.
        ptys: The pty bridges the machine had after the build.
    """

    machine: Machine
    snapshot: Snapshot
    saved: bytes
    boot_stop: Stop | None
    image_count: int
    ptys: dict[str, str]
    _decoded: Snapshot | None = None

    def decoded(self) -> Snapshot:
        """`saved` read back, decoded once."""
        if self._decoded is None:
            self._decoded = Snapshot.from_bytes(self.saved)
        return self._decoded


@dataclass(slots=True)
class PopcornSession:
    """The plugin's state in one process, one per xdist worker."""

    settings: Settings
    loop: OwnedLoop
    boots: int = 0
    rebuilds: int = 0
    _projects: dict[str | None, Project] = field(default_factory=dict[str | None, Project])
    _booted: dict[Key, Booted] = field(default_factory=dict[Key, Booted])
    _failed: dict[Key, BaseException] = field(default_factory=dict[Key, BaseException])
    _locks: dict[Key, asyncio.Lock] = field(default_factory=dict[Key, asyncio.Lock])

    @property
    def keys(self) -> list[Key]:
        """The keys booted or tried in this process."""
        return [*self._booted, *self._failed]

    def project(self, config: str | None = None) -> Project:
        """The project file with `config` applied, loaded once per config.

        Raises:
            pytest.fail.Exception: No project file exists.
            ConfigError: The file is invalid or has no such config.
        """
        if config not in self._projects:
            path = self.settings.project
            if not path.is_file():
                pytest.fail(
                    f"no project file {path}: pass --popcorn-project or add popcorn.toml at "
                    "<rootdir>",
                    pytrace=False,
                )
            self._projects[config] = Project.load(path, config)
        return self._projects[config]

    def key(self, config: str | None, strict: bool | None) -> Key:
        """The key of `config`, strictness from its project file when `strict` is None."""
        if strict is None:
            strict = self.project(config).file.models.strict
        return Key(config, strict)

    def build(self, key: Key) -> Machine:
        """A new machine of `key`, not reset."""
        return self.project(key.config).machine(strict_peripherals=key.strict)

    def booted(self, key: Key) -> Booted:
        """The booted state of `key`, booting it on the owned loop the first time.

        Raises:
            RuntimeError: `key` is not booted yet and an event loop is running.
            Exception: The boot of `key` failed, now or earlier in the session: a fresh copy of
                that error.
        """
        found = self._lookup(key)
        if found is not None:
            return found
        try:
            asyncio.get_running_loop()
        except RuntimeError:
            return self.loop.run(self._boot(key), contextvars.copy_context())
        raise RuntimeError(
            f"the popcorn boot of {key} runs from sync fixture setup or "
            "`await machine_factory.booted(...)`, not from inside a running event loop"
        )

    async def booted_async(self, key: Key) -> Booted:
        """The booted state of `key`, booting it on the running loop the first time. Concurrent
        callers of one key wait for a single boot."""
        found = self._lookup(key)
        if found is not None:
            return found
        lock = self._locks.setdefault(key, asyncio.Lock())
        async with lock:
            found = self._lookup(key)
            if found is not None:
                return found
            try:
                return await self._boot(key)
            finally:
                self._locks.pop(key, None)

    def rebuild(self, key: Key) -> Booted:
        """Replace the reused machine of `key` by a new one restored from the saved form."""
        old = self._booted[key]
        old.machine.board.close()
        machine = self.build(key)
        machine.restore(old.decoded())
        booted = Booted(
            machine=machine,
            snapshot=machine.snapshot(),
            saved=old.saved,
            boot_stop=old.boot_stop,
            image_count=len(machine.images),
            ptys=machine.board.ptys,
        )
        self._booted[key] = booted
        self.rebuilds += 1
        return booted

    def close(self) -> None:
        """Close the pty bridges of the reused machines."""
        for booted in self._booted.values():
            booted.machine.board.close()

    def _lookup(self, key: Key) -> Booted | None:
        failed = self._failed.get(key)
        if failed is not None:
            raise fresh_copy(failed) from failed
        return self._booted.get(key)

    async def _boot(self, key: Key) -> Booted:
        project = self.project(key.config)
        self.boots += 1
        try:
            machine = self.build(key)
            image_count = len(machine.images)
            ptys = machine.board.ptys
            if project.file.boot.until is None:
                machine.reset()
                stop = None
            else:
                stop = await project.boot(machine)
            snapshot = machine.snapshot()
        except Exception as error:
            self._failed[key] = error
            raise
        booted = Booted(
            machine=machine,
            snapshot=snapshot,
            saved=snapshot.to_bytes(),
            boot_stop=stop,
            image_count=image_count,
            ptys=ptys,
        )
        self._booted[key] = booted
        return booted


def fresh_copy(error: BaseException) -> BaseException:
    """A new exception equal to `error`, without its traceback or cause, its notes copied."""
    try:
        fresh = copy.copy(error)
    except TypeError:
        fresh = type(error).__new__(type(error))
        fresh.args = error.args
        fresh.__dict__.update(error.__dict__)
    fresh.__notes__ = list(getattr(error, "__notes__", ()))
    return fresh


SESSION = pytest.StashKey[PopcornSession]()


def popcorn_session(config: pytest.Config) -> PopcornSession:
    """The plugin state of `config`, created on first use."""
    session = config.stash.get(SESSION, None)
    if session is None:
        session = config.stash[SESSION] = PopcornSession(config.stash[SETTINGS], owned_loop(config))
    return session


def summary(session: PopcornSession) -> dict[str, object]:
    """The counters of `session`, as a worker hands them to the xdist controller."""
    return {"keys": [str(key) for key in session.keys], "boots": session.boots, "cache_hits": 0}


def pytest_sessionfinish(session: pytest.Session) -> None:
    """Close the pty bridges and hand the counters to the xdist controller."""
    state = session.config.stash.get(SESSION, None)
    if state is None:
        return
    state.close()
    workeroutput: object = getattr(session.config, "workeroutput", None)
    if isinstance(workeroutput, dict):
        workeroutput["popcorn"] = summary(state)


def pytest_terminal_summary(terminalreporter: pytest.TerminalReporter) -> None:
    """One line of boot counters when a machine was booted."""
    state = terminalreporter.config.stash.get(SESSION, None)
    if state is None or not state.keys:
        return
    terminalreporter.write_line(
        f"popcorn: {len(state.keys)} key(s), {state.boots} boot(s), 0 from cache"
    )
