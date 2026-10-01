"""Per-process plugin state: the project, one booted machine per key, the boot counters."""

from __future__ import annotations

import asyncio
import contextvars
import copy
import dataclasses
import hashlib
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import cast

import pytest

from popcorn import ConfigError, Machine, Project, Snapshot, Stop
from popcorn._devices import serving
from popcorn._machine import AttachedDevice
from popcorn.coverage import Coverage, worker_path
from popcorn.pytest._cache import Entry, cache_directory, cache_key, devices_digest, locked
from popcorn.pytest._loop import OwnedLoop, owned_loop
from popcorn.pytest._options import SETTINGS, Settings

type Devices = Mapping[str, Sequence[AttachedDevice]]
"""Bus devices per peripheral, as `Project.machine(devices=)` takes them."""

UNCACHED = "uncached"
"""`Key.devices` of a device set that cannot be keyed: every boot of it is a new one."""


@dataclass(frozen=True, slots=True)
class Key:
    """What a booted machine depends on besides the project file.

    Attributes:
        config: The `[configs.NAME]` applied, None for the base file.
        strict: Whether unmodelled peripheral accesses raise.
        devices: The digest of the bus devices given instead of the project file's, UNCACHED
            when they cannot be keyed, None for the project file's.
    """

    config: str | None
    strict: bool
    devices: str | None = None

    def __str__(self) -> str:
        base = f"config={self.config or '-'} strict={self.strict}"
        return base if self.devices is None else f"{base} devices={self.devices}"


@dataclass(slots=True)
class Booted:
    """The booted state of one key.

    Attributes:
        machine: The machine the `machine` fixture restores for every test of the key.
        snapshot: The boot snapshot, taken on `machine`.
        saved: `snapshot` in its saved form, restorable into any machine of the key.
        boot_stop: The stop of the boot, None for a project without `[boot] until`. Read back
            from the cache entry on a cache hit, located in this machine.
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


@dataclass(frozen=True, slots=True)
class CoverageRun:
    """Where this process saves firmware coverage for `--popcorn-cov`.

    Attributes:
        directory: The session directory of `.pcov` files, shared by xdist workers.
        worker: This process's xdist worker id, `main` without xdist.
        owner: Whether this process made the directory, merges it and removes it.
    """

    directory: Path
    worker: str
    owner: bool

    @property
    def path(self) -> Path:
        """This process's `.pcov` file."""
        return worker_path(self.directory, self.worker)


COVERAGE = pytest.StashKey[CoverageRun]()


@dataclass(slots=True)
class PopcornSession:
    """The plugin's state in one process, one per xdist worker."""

    settings: Settings
    loop: OwnedLoop
    coverage: CoverageRun | None = None
    cache: Path | None = None
    coverage_failures: list[str] = field(default_factory=list[str])
    boots: int = 0
    cache_hits: int = 0
    cache_dropped: int = 0
    rebuilds: int = 0
    _projects: dict[str | None, Project] = field(default_factory=dict[str | None, Project])
    _booted: dict[Key, Booted] = field(default_factory=dict[Key, Booted])
    _failed: dict[Key, BaseException] = field(default_factory=dict[Key, BaseException])
    _locks: dict[Key, asyncio.Lock] = field(default_factory=dict[Key, asyncio.Lock])
    _failed_machines: dict[Key, Machine] = field(default_factory=dict[Key, Machine])
    _device_boots: dict[Key, Snapshot] = field(default_factory=dict[Key, Snapshot])
    _device_keys: set[Key] = field(default_factory=set[Key])

    @property
    def keys(self) -> list[Key]:
        """The keys booted or tried in this process."""
        return [*self._booted, *self._failed, *self._device_keys]

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

    def device_key(self, key: Key, devices: Devices) -> Key:
        """`key` with the bus devices `devices` given instead of the project file's.

        Raises:
            ConfigError: A device serves another live machine.
        """
        for name, targets in devices.items():
            for target in targets:
                if serving(target) is not None:
                    raise ConfigError(
                        f"devices.{name}: {target!r} not serving another live machine",
                        key=f"devices.{name}",
                        expected=f"{target!r} not serving another live machine",
                    )
        digest = devices_digest(self.project(key.config), devices)
        return dataclasses.replace(key, devices=UNCACHED if digest is None else digest)

    def build(self, key: Key, devices: Devices | None = None) -> Machine:
        """A new machine of `key` with `devices`, not reset, counting coverage under
        `--popcorn-cov`."""
        machine = self.project(key.config).machine(strict_peripherals=key.strict, devices=devices)
        if self.coverage is not None:
            machine.coverage.enable()
        return machine

    def collect(self, machine: Machine) -> None:
        """Add the coverage `machine` counted to this process's `.pcov` file. A file that cannot
        be written is a coverage failure, reported at session end, not an error of the test."""
        if self.coverage is None:
            return
        try:
            machine.coverage.merge_into(self.coverage.path)
        except OSError as error:
            self.coverage_failures.append(f"coverage not saved to {self.coverage.path}: {error}")

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

    def booted_with(self, key: Key, devices: Devices) -> tuple[Machine, Snapshot]:
        """A machine of `key`, a `device_key`, built with `devices`, and the boot snapshot to
        restore into it, booting on the owned loop when this process has no boot of `key`.

        Raises:
            RuntimeError: A boot is needed and an event loop is running.
        """
        found = self._device_boots.get(key)
        if found is not None:
            return self.build(key, devices), found
        try:
            asyncio.get_running_loop()
        except RuntimeError:
            return self.loop.run(self.booted_with_async(key, devices), contextvars.copy_context())
        raise RuntimeError(
            f"the popcorn boot of {key} runs from sync fixture setup or "
            "`await machine_factory.booted(...)`, not from inside a running event loop"
        )

    async def booted_with_async(self, key: Key, devices: Devices) -> tuple[Machine, Snapshot]:
        """As `booted_with`, booting on the running loop. A boot whose devices cannot be keyed
        is never shared: each one boots."""
        found = self._device_boots.get(key)
        if found is not None:
            return self.build(key, devices), found
        self._device_keys.add(key)
        project = self.project(key.config)
        if key.devices == UNCACHED:
            booted = await self._fresh_boot(key, project, devices)
            return booted.machine, booted.decoded()
        lock = self._locks.setdefault(key, asyncio.Lock())
        async with lock:
            found = self._device_boots.get(key)
            if found is not None:
                return self.build(key, devices), found
            try:
                booted = await self._cached_boot(key, project, devices)
            finally:
                self._locks.pop(key, None)
        self._device_boots[key] = booted.decoded()
        return booted.machine, booted.decoded()

    def rebuild(self, key: Key) -> Booted:
        """Replace the reused machine of `key` by a new one restored from the saved form."""
        old = self._booted[key]
        self.collect(old.machine)
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

    def booted_machines(self) -> list[Machine]:
        """The reused machine of every key booted in this process."""
        return [booted.machine for booted in self._booted.values()]

    def failed_machine(self, key: Key) -> Machine | None:
        """The machine whose boot of `key` failed, as the failure left it, None when the build
        itself failed or the boot did not."""
        return self._failed_machines.get(key)

    def close(self) -> None:
        """Close the pty bridges of the reused machines and of those whose boot failed."""
        for booted in self._booted.values():
            booted.machine.board.close()
        for machine in self._failed_machines.values():
            machine.board.close()

    def _save_boot_coverage(self, key: Key, machine: Machine, snapshot: Snapshot) -> None:
        assert self.coverage is not None
        boot = machine.coverage.collect()
        machine.coverage.clear()
        path = boot_coverage_path(self.coverage.directory, key, snapshot)
        try:
            boot.save(path)
        except OSError as error:
            self.coverage_failures.append(f"boot coverage of {key} not saved: {error}")

    def _lookup(self, key: Key) -> Booted | None:
        failed = self._failed.get(key)
        if failed is not None:
            raise fresh_copy(failed) from failed
        return self._booted.get(key)

    async def _boot(self, key: Key) -> Booted:
        project = self.project(key.config)
        try:
            booted = await self._cached_boot(key, project)
        except Exception as error:
            self._failed[key] = error
            raise
        self._booted[key] = booted
        return booted

    async def _cached_boot(
        self, key: Key, project: Project, devices: Devices | None = None
    ) -> Booted:
        if self.cache is None:
            return await self._fresh_boot(key, project, devices)
        self.cache.mkdir(parents=True, exist_ok=True)
        entry = Entry.of(self.cache, cache_key(project, key.strict, key.devices))
        async with locked(entry):
            if entry.exists(self.coverage is not None):
                restored = self._restored_from(key, entry, devices)
                if restored is not None:
                    return restored
            booted = await self._fresh_boot(key, project, devices)
            entry.save(booted.snapshot, booted.boot_stop, self._boot_coverage_bytes(key, booted))
            return booted

    def _restored_from(
        self, key: Key, entry: Entry, devices: Devices | None = None
    ) -> Booted | None:
        """The boot of `key` restored from `entry`, None when the entry cannot be used: it is
        dropped and the key boots. Any error falls back to the boot, a Python model's `restore`
        raising its own exception type included."""
        machine: Machine | None = None
        try:
            machine = self.build(key, devices)
            ptys = machine.board.ptys
            machine.restore(Snapshot.load(entry.snapshot))
            snapshot = machine.snapshot()
            stop = entry.stop(machine)
            if self.coverage is not None:
                path = boot_coverage_path(self.coverage.directory, key, snapshot)
                Coverage.load(entry.coverage).save(path)
        except Exception:
            if machine is not None:
                machine.board.close()
            entry.drop()
            self.cache_dropped += 1
            return None
        self.cache_hits += 1
        return Booted(
            machine=machine,
            snapshot=snapshot,
            saved=snapshot.to_bytes(),
            boot_stop=stop,
            image_count=len(machine.images),
            ptys=ptys,
        )

    def _boot_coverage_bytes(self, key: Key, booted: Booted) -> bytes | None:
        if self.coverage is None:
            return None
        path = boot_coverage_path(self.coverage.directory, key, booted.snapshot)
        return path.read_bytes() if path.is_file() else None

    async def _fresh_boot(
        self, key: Key, project: Project, devices: Devices | None = None
    ) -> Booted:
        self.boots += 1
        machine: Machine | None = None
        try:
            machine = self.build(key, devices)
            image_count = len(machine.images)
            ptys = machine.board.ptys
            if project.file.boot.until is None:
                machine.reset()
                stop = None
            else:
                stop = await project.boot(machine)
            snapshot = machine.snapshot()
            if self.coverage is not None:
                self._save_boot_coverage(key, machine, snapshot)
        except Exception:
            if machine is not None:
                self._failed_machines[key] = machine
            raise
        return Booted(
            machine=machine,
            snapshot=snapshot,
            saved=snapshot.to_bytes(),
            boot_stop=stop,
            image_count=image_count,
            ptys=ptys,
        )


def boot_coverage_path(directory: Path, key: Key, snapshot: Snapshot) -> Path:
    """The file of the boot coverage of `key`: the same name in every process booting it, from
    the key and the digests of its boot snapshot."""
    identity = "\n".join(
        (str(key), snapshot.description_digest, *snapshot.image_digests, snapshot.config_digest)
    )
    return directory / f"boot-{hashlib.sha256(identity.encode()).hexdigest()[:16]}.pcov"


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
        session = config.stash[SESSION] = PopcornSession(
            config.stash[SETTINGS],
            owned_loop(config),
            config.stash.get(COVERAGE, None),
            cache_directory(config),
        )
    return session


def summary(session: PopcornSession) -> dict[str, object]:
    """The counters of `session`, as a worker hands them to the xdist controller."""
    return {
        "keys": [str(key) for key in session.keys],
        "boots": session.boots,
        "cache_hits": session.cache_hits,
        "cache_dropped": session.cache_dropped,
        "coverage_failures": list(session.coverage_failures),
    }


def pytest_sessionfinish(session: pytest.Session) -> None:
    """Close the pty bridges and hand the counters to the xdist controller."""
    state = session.config.stash.get(SESSION, None)
    if state is None:
        return
    for booted in state.booted_machines():
        state.collect(booted)
    state.close()
    workeroutput: object = getattr(session.config, "workeroutput", None)
    if isinstance(workeroutput, dict):
        workeroutput["popcorn"] = summary(state)


WORKER_SUMMARIES = pytest.StashKey[list[dict[str, object]]]()


def summary_line(summaries: list[dict[str, object]]) -> str | None:
    """The one terminal line of the summed counters of `summaries`, None when nothing booted."""
    keys: set[str] = set()
    boots = hits = 0
    for one in summaries:
        keys.update(str(key) for key in cast(list[object], one.get("keys", [])))
        boots += cast(int, one.get("boots", 0))
        hits += cast(int, one.get("cache_hits", 0))
    if not keys:
        return None
    return f"popcorn: {len(keys)} key(s), {boots} boot(s), {hits} from cache"


def pytest_terminal_summary(terminalreporter: pytest.TerminalReporter) -> None:
    """One line of boot counters, summed over xdist workers on the controller."""
    config = terminalreporter.config
    summaries = list(config.stash.get(WORKER_SUMMARIES, list[dict[str, object]]()))
    state = config.stash.get(SESSION, None)
    if state is not None:
        summaries.append(summary(state))
    line = summary_line(summaries)
    if line is not None:
        terminalreporter.write_line(line)
    dropped = sum(cast(int, one.get("cache_dropped", 0)) for one in summaries)
    if dropped:
        terminalreporter.write_line(
            f"popcorn: {dropped} unreadable boot cache entr{'y' if dropped == 1 else 'ies'} "
            "dropped and booted again"
        )
