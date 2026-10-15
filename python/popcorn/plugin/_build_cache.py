"""The dev-mode build cache of native plugins, shared by processes through file locks.

An entry is `<root>/<name>-<key>/`: `src/` a copy of the plugin directory without the files
IGNORED names, `build/` the pcons build of that copy, `lib/` its outputs and their manifest. The
key covers the ABI tag, the native code popcorn runs, `sdk.json` and the build helper modules, and
every file of the copy, so an edit gives a new entry at a new path. A library is never unloaded, a
changed file at an old path would `dlopen` to the old handle.
"""

from __future__ import annotations

import contextlib
import fcntl
import fnmatch
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import sysconfig
import tempfile
import time
from collections.abc import Generator
from dataclasses import dataclass
from pathlib import Path

from popcorn import _core
from popcorn._errors import ConfigError
from popcorn._native_digest import native_digest
from popcorn.plugin._sdk import MANIFEST, Sdk, package_directory, sdk

CACHE_VARIABLE = "POPCORN_PLUGIN_CACHE"
BUILD_SCRIPT = "popcorn-plugin.py"
KEPT = 2
LOG_TAIL = 60
STAGING_AGE = 3600.0
MAX_JOBS = 6
IGNORED = (".*", "__pycache__", "*.pyc", "*~", "#*#")

_IGNORED_VARIABLES = {"CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LDLIBS"}
_VERSION_MACRO = re.compile(r'^#define __VERSION__ "(.*)"$', re.MULTILINE)


@dataclass(frozen=True, slots=True)
class Built:
    """The outputs of one plugin build.

    Attributes:
        key: The cache key the entry is named by.
        name: The name the build script gave `PluginProject`.
        library: The plugin library.
        bindings: Its bindings module, None without one.
    """

    key: str
    name: str
    library: Path
    bindings: Path | None


_keys: dict[tuple[str, Path], str] = {}
_in_use: dict[Path, int] = {}


def cache_root() -> Path:
    """`$POPCORN_PLUGIN_CACHE`, else `$XDG_CACHE_HOME/popcorn/plugins`, else
    `~/.cache/popcorn/plugins`."""
    if root := os.environ.get(CACHE_VARIABLE):
        return Path(root)
    if xdg := os.environ.get("XDG_CACHE_HOME"):
        return Path(xdg) / "popcorn" / "plugins"
    return Path.home() / ".cache" / "popcorn" / "plugins"


def ignored(name: str) -> bool:
    """Whether a file or directory named @p name is left out of a plugin's copy and its key:
    hidden, editor swap or backup files and Python caches, matching IGNORED."""
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in IGNORED)


def _ignored_names(directory: str, names: list[str]) -> set[str]:
    return {name for name in names if ignored(name)}


def tree_digest(root: Path) -> str:
    """SHA-256 over every file under @p root, relative path and content, sorted by path, leaving
    out the files and directories `ignored` names."""
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        if path.is_file() and not any(ignored(part) for part in path.relative_to(root).parts):
            content = path.read_bytes()
            digest.update(f"{path.relative_to(root).as_posix()}\0{len(content)}\0".encode())
            digest.update(content)
    return digest.hexdigest()


def helper_files() -> tuple[Path, ...]:
    """The files of popcorn a plugin build reads besides the SDK headers, which the ABI tag
    covers: `sdk.json` and the modules `PluginProject` builds from."""
    plugin = Path(__file__).resolve().parent
    return (package_directory() / "sdk" / "sdk.json", plugin / "build.py", plugin / "_sdk.py")


def _helpers_digest() -> str:
    digest = hashlib.sha256()
    for path in helper_files():
        content = path.read_bytes()
        digest.update(f"{path.name}\0{len(content)}\0".encode())
        digest.update(content)
    return digest.hexdigest()


def key_of(copy: Path) -> str:
    """The cache key of the plugin copy at @p copy."""
    digest = hashlib.sha256()
    abi_tag = _core._host_abi_tag()  # pyright: ignore[reportPrivateUsage]
    for part in (abi_tag, native_digest(), _helpers_digest(), tree_digest(copy)):
        digest.update(part.encode())
        digest.update(b"\0")
    return digest.hexdigest()


def known_key(name: str, source: Path) -> str | None:
    """The key of the first build this process returned for the plugin @p name at @p source,
    None before. A failed build records no key."""
    return _keys.get((name, source.resolve()))


def require_tools() -> None:
    """Raise ConfigError when pcons or ninja is missing."""
    if importlib.util.find_spec("pcons") is None or shutil.which("ninja") is None:
        raise ConfigError(
            "dev-mode plugins need pcons and ninja",
            key="plugin",
            expected="pcons importable and ninja on PATH",
        )


def compiler_identity(compiler: str) -> tuple[str, str]:
    """`-dumpfullversion` and `__VERSION__` of @p compiler, empty when it cannot run."""
    try:
        version = subprocess.run(
            [compiler, "-dumpfullversion"], capture_output=True, text=True, check=False
        ).stdout.strip()
        macros = subprocess.run(
            [compiler, "-dM", "-E", "-x", "c++", "/dev/null"],
            capture_output=True,
            text=True,
            check=False,
        ).stdout
    except OSError:
        return "", ""
    found = _VERSION_MACRO.search(macros)
    return version, found.group(1) if found else ""


def require_compiler(toolkit: Sdk) -> None:
    """Raise ConfigError when the SDK's compiler is not the build popcorn was compiled with."""
    found = compiler_identity(toolkit.compiler)
    expected = (toolkit.compiler_version, toolkit.compiler_ident)
    if found != expected:
        raise ConfigError(
            f"plugin compiler {toolkit.compiler} is {found[0] or 'not runnable'}"
            f" ({found[1] or 'no __VERSION__'}), popcorn was built with {expected[0]}"
            f" ({expected[1]}): plugins need the same GCC build popcorn was built with",
            file=toolkit.compiler,
            key="compiler",
            expected=f"g++ {expected[0]}, __VERSION__ {expected[1]}",
        )


def _open_locked(path: Path, operation: int) -> int | None:
    """A descriptor of @p path holding the `flock` @p operation, None when `LOCK_NB` finds it
    held. Eviction unlinks lock files, so a lock taken on a file the path no longer names is
    dropped and taken again on the file it names now."""
    while True:
        descriptor = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
        try:
            fcntl.flock(descriptor, operation)
        except BlockingIOError:
            os.close(descriptor)
            return None
        try:
            current = os.path.samestat(os.fstat(descriptor), os.stat(path))
        except FileNotFoundError:
            current = False
        if current:
            return descriptor
        os.close(descriptor)


@contextlib.contextmanager
def locked(path: Path) -> Generator[None]:
    """Hold an exclusive `flock` on @p path, waiting for it. Unreliable on NFS."""
    descriptor = _open_locked(path, fcntl.LOCK_EX)
    assert descriptor is not None
    try:
        yield
    finally:
        os.close(descriptor)


def _hold(entry: Path) -> bool:
    """Take a shared lock on @p entry's use file for the life of the process, so eviction by
    another process skips it. False when this process held it already."""
    if entry in _in_use:
        return False
    descriptor = _open_locked(Path(f"{entry}.use"), fcntl.LOCK_SH)
    assert descriptor is not None
    _in_use[entry] = descriptor
    return True


def build_environment() -> dict[str, str]:
    """This process's environment without compiler, flag or pcons variables, with popcorn
    importable by the build script."""
    environment = {
        key: value
        for key, value in os.environ.items()
        if key not in _IGNORED_VARIABLES and not key.startswith("PCONS_")
    }
    path = str(package_directory().parent)
    previous = environment.get("PYTHONPATH")
    environment["PYTHONPATH"] = path if not previous else f"{path}{os.pathsep}{previous}"
    return environment


def build_command(source: Path, build: Path) -> list[str]:
    """The pcons command building the plugin in @p source into @p build, at most MAX_JOBS jobs."""
    jobs = min(os.cpu_count() or 1, MAX_JOBS)
    return [
        sys.executable,
        "-m",
        "pcons",
        "-C",
        str(source),
        "-b",
        BUILD_SCRIPT,
        "-B",
        str(build),
        "-j",
        str(jobs),
    ]


def run_build(source: Path, build: Path) -> subprocess.CompletedProcess[str]:
    """Run the plugin's `popcorn-plugin.py` in @p source with pcons, outputs in @p build."""
    return subprocess.run(
        build_command(source, build),
        capture_output=True,
        text=True,
        env=build_environment(),
        check=False,
    )


def _search_directories(compiler: str) -> list[Path]:
    """The compiler's own include directories."""
    ran = subprocess.run(
        [compiler, "-E", "-Wp,-v", "-x", "c++", "/dev/null"],
        capture_output=True,
        text=True,
        check=False,
    )
    return [
        Path(line.strip()).resolve() for line in ran.stderr.splitlines() if line.startswith(" /")
    ]


def outside_includes(build: Path, allowed: list[Path]) -> list[Path]:
    """Inputs of the objects of @p build, from `ninja -t deps`, under none of @p allowed."""
    ran = subprocess.run(
        ["ninja", "-C", str(build), "-t", "deps"], capture_output=True, text=True, check=True
    )
    outside: list[Path] = []
    for line in ran.stdout.splitlines():
        if not line.startswith("    "):
            continue
        path = (build / line.strip()).resolve()
        if not any(path.is_relative_to(root) for root in allowed) and path not in outside:
            outside.append(path)
    return outside


def _tail(text: str) -> str:
    return "\n".join(text.splitlines()[-LOG_TAIL:])


def _failure(entry: Path, name: str, message: str) -> ConfigError:
    marker = Path(f"{entry}.failed")
    marker.write_text(message)
    return ConfigError(
        message, file=str(marker), key=f"plugins.{name}", expected="a plugin that builds"
    )


def _build_entry(entry: Path, name: str, toolkit: Sdk) -> None:
    """Build `entry/src` into `entry/build`, check its includes, copy outputs to `entry/lib`."""
    source = entry / "src"
    build = entry / "build"
    ran = run_build(source, build)
    if ran.returncode != 0:
        raise _failure(
            entry, name, f"plugin {name}: the build failed\n{_tail(ran.stdout + ran.stderr)}"
        )
    allowed = [
        source.resolve(),
        toolkit.include.resolve(),
        Path(sysconfig.get_paths()["include"]).resolve(),
        *_search_directories(toolkit.compiler),
    ]
    if outside := outside_includes(build, allowed):
        raise _failure(
            entry,
            name,
            f"plugin {name}: {outside[0]} is outside the plugin directory, a plugin's sources"
            " live in its directory",
        )
    manifest: dict[str, object] = json.loads((build / MANIFEST).read_text())
    lib = entry / "lib"
    lib.mkdir()
    library = str(manifest["library"])
    shutil.copy2(build / library, lib / library)
    bindings = manifest.get("bindings")
    if isinstance(bindings, str):
        (lib / bindings).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(build / bindings, lib / bindings)
    part = lib / f"{MANIFEST}.part"
    part.write_text(json.dumps(manifest))
    part.replace(lib / MANIFEST)


def _built(entry: Path, key: str) -> Built:
    manifest: dict[str, object] = json.loads((entry / "lib" / MANIFEST).read_text())
    bindings = manifest.get("bindings")
    return Built(
        key=key,
        name=str(manifest["name"]),
        library=entry / "lib" / str(manifest["library"]),
        bindings=entry / "lib" / bindings if isinstance(bindings, str) else None,
    )


def evict(root: Path, name: str) -> None:
    """Delete the entries of @p name beyond the KEPT newest, skipping one a process uses or
    builds, and staging copies of @p name older than STAGING_AGE, left by a killed process."""
    for staging in root.glob(f".{name}-copy-*"):
        with contextlib.suppress(OSError):
            if time.time() - staging.stat().st_mtime > STAGING_AGE:
                shutil.rmtree(staging, ignore_errors=True)
    keys: dict[str, float] = {}
    for path in root.glob(f"{name}-*"):
        key = path.name.removeprefix(f"{name}-").split(".")[0]
        if len(key) == 64:
            with contextlib.suppress(FileNotFoundError):
                keys[key] = max(keys.get(key, 0.0), path.stat().st_mtime)
    for key in sorted(keys, key=keys.__getitem__, reverse=True)[KEPT:]:
        entry = root / f"{name}-{key}"
        use = _open_locked(Path(f"{entry}.use"), fcntl.LOCK_EX | fcntl.LOCK_NB)
        if use is None:
            continue
        try:
            lock = _open_locked(Path(f"{entry}.lock"), fcntl.LOCK_EX | fcntl.LOCK_NB)
            if lock is None:
                continue
            try:
                shutil.rmtree(entry, ignore_errors=True)
                for suffix in (".failed", ".lock", ".use"):
                    Path(f"{entry}{suffix}").unlink(missing_ok=True)
            finally:
                os.close(lock)
        finally:
            os.close(use)


def build(name: str, source: Path) -> Built:
    """The build of the plugin @p name in the directory @p source, from the cache or built now.

    Raises:
        ConfigError: pcons or ninja is missing, the compiler is not popcorn's, the directory
            has no `popcorn-plugin.py`, the cache root is inside it, its sources changed since
            this process first built it, the build fails, now or in an earlier process, or it
            includes a file outside the directory.
    """
    require_tools()
    toolkit = sdk()
    require_compiler(toolkit)
    source = source.resolve()
    if not (source / BUILD_SCRIPT).is_file():
        raise ConfigError(
            f"plugin {name}: {source} has no {BUILD_SCRIPT}",
            file=str(source),
            key=f"plugins.{name}",
            expected=f"a plugin directory holding {BUILD_SCRIPT}",
        )
    root = cache_root()
    if root.resolve().is_relative_to(source):
        raise ConfigError(
            f"plugin {name}: the plugin cache {root} is inside the plugin directory {source}",
            file=str(root),
            key=CACHE_VARIABLE,
            expected="a plugin cache outside every plugin directory",
        )
    root.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=f".{name}-copy-", dir=root))
    try:
        copy = staging / "src"
        shutil.copytree(source, copy, ignore=_ignored_names)
        key = key_of(copy)
        first = _keys.get((name, source), key)
        if first != key:
            raise ConfigError(
                f"plugin {name}: the sources in {source} changed after this process built them,"
                " restart the process to load the edited plugin",
                file=str(source),
                key=f"plugins.{name}",
                expected="the sources this process built, a loaded plugin is never unloaded",
            )
        entry = root / f"{name}-{key}"
        built_now = False
        held_now = _hold(entry)
        try:
            with locked(Path(f"{entry}.lock")):
                failed = Path(f"{entry}.failed")
                if failed.is_file():
                    raise ConfigError(
                        failed.read_text(),
                        file=str(failed),
                        key=f"plugins.{name}",
                        expected="a plugin that builds",
                    )
                if not (entry / "lib" / MANIFEST).is_file():
                    shutil.rmtree(entry, ignore_errors=True)
                    entry.mkdir()
                    copy.rename(entry / "src")
                    _build_entry(entry, name, toolkit)
                    built_now = True
        except BaseException:
            if held_now:
                os.close(_in_use.pop(entry))
            raise
        _keys[(name, source)] = key
        if built_now:
            evict(root, name)
        return _built(entry, key)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
