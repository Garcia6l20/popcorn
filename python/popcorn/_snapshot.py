"""Snapshots: the whole machine state, restorable in place or saved to a file."""

from __future__ import annotations

import contextlib
import os
import tempfile
from pathlib import Path

from popcorn import _core
from popcorn._errors import ConfigError


def _read_umask() -> int:
    mask = os.umask(0)
    os.umask(mask)
    return mask


_UMASK = _read_umask()


class Snapshot:
    """The whole state of a machine at one instruction boundary.

    Restorable any number of times into the machine that took it. Its saved form, `save()`,
    loads into another machine built from the same description, images and configuration.
    """

    __slots__ = ("_arena_offset", "_native")

    def __init__(self, native: _core.SnapshotHandle, arena_offset: int = 0) -> None:
        """Wrap a native snapshot. Use Machine.snapshot or Snapshot.load instead."""
        self._native = native
        self._arena_offset = arena_offset

    @property
    def arena_offset(self) -> int:
        """Call arena bytes allocated when it was taken, 0 for one read back from a file."""
        return self._arena_offset

    @property
    def native(self) -> _core.SnapshotHandle:
        """The native snapshot."""
        return self._native

    @property
    def machine_id(self) -> int:
        """Identity of the machine that took it, 0 when read back from a file."""
        return self._native.machine_id()

    @property
    def description_digest(self) -> str:
        """Digest of the description it was taken on."""
        return self._native.description_digest()

    @property
    def image_digests(self) -> tuple[str, ...]:
        """Digest of each image loaded when it was taken, in load order."""
        return tuple(self._native.image_digests())

    @property
    def config_digest(self) -> str:
        """Configuration digest of the machine that took it."""
        return self._native.config_digest()

    def to_bytes(self) -> bytes:
        """The saved form, BSON."""
        return self._native.encode()

    def save(self, path: str | os.PathLike[str]) -> None:
        """Write the saved form to `path`, through a synced temporary file in the same directory
        renamed over it, so that a reader never sees half a file, even after a crash. The file
        gets the permissions the umask gives a new file."""
        destination = Path(path)
        data = self.to_bytes()
        handle, name = tempfile.mkstemp(dir=destination.parent, prefix=f"{destination.name}.tmp.")
        temporary = Path(name)
        try:
            try:
                out = os.fdopen(handle, "wb")
            except BaseException:
                os.close(handle)
                raise
            with out:
                out.write(data)
                out.flush()
                os.fsync(out.fileno())
            temporary.chmod(0o666 & ~_UMASK)
            temporary.replace(destination)
        except BaseException:
            with contextlib.suppress(FileNotFoundError):
                temporary.unlink()
            raise
        directory = os.open(destination.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)

    @classmethod
    def from_bytes(cls, data: bytes) -> Snapshot:
        """Read a saved form.

        Raises:
            ConfigError: The bytes are malformed or of another format version.
        """
        return cls(_core.decode_snapshot(data))

    @classmethod
    def load(cls, path: str | os.PathLike[str]) -> Snapshot:
        """Read a saved form from a file.

        Raises:
            ConfigError: The file is malformed or of another format version.
        """
        try:
            return cls.from_bytes(Path(path).read_bytes())
        except ConfigError as error:
            raise ConfigError(
                str(error),
                file=str(path),
                line=error.line,
                key=error.key,
                expected=error.expected,
            ) from error

    def __repr__(self) -> str:
        return f"Snapshot(machine={self.machine_id}, images={len(self.image_digests)})"
