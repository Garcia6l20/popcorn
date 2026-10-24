"""Dev-mode plugin builds: the content-addressed cache, its lock, its refusals and its bound."""

from __future__ import annotations

import dataclasses
import fcntl
import os
import shutil
import subprocess
import sys
import textwrap
import time
from pathlib import Path

import pytest
from elftools.elf.elffile import ELFFile
from popcorn import ConfigError, _core
from popcorn.plugin import Plugin, _build_cache
from popcorn.plugin._records import constructed
from popcorn.plugin._sdk import sdk

from .support import copied, entries, run_python


@pytest.fixture
def cache(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    root = tmp_path / "cache"
    monkeypatch.setenv("POPCORN_PLUGIN_CACHE", str(root))
    return root


def abi_tag(library: Path) -> str:
    with library.open("rb") as stream:
        section = ELFFile(stream).get_section_by_name(".popcorn_abi")
        assert section is not None
        data: bytes = section.data()
    return data.split(b"\0")[0].decode()


def no_build(source: Path, build: Path) -> subprocess.CompletedProcess[str]:
    raise AssertionError(f"built {source} into {build}")


def test_a_fresh_cache_builds_a_library_with_the_host_tag(tmp_path: Path, cache: Path) -> None:
    library = Plugin.dev(copied("echo", tmp_path)).built()
    assert library.is_file()
    assert library.is_relative_to(cache)
    assert abi_tag(library) == _core._host_abi_tag()  # pyright: ignore[reportPrivateUsage]


def test_a_second_build_without_a_change_runs_nothing(
    tmp_path: Path, cache: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    plugin = Plugin.dev(copied("echo", tmp_path))
    library = plugin.built()
    (entry,) = entries(cache, "echo")
    before = entry.stat().st_mtime_ns, library.stat().st_mtime_ns
    monkeypatch.setattr(_build_cache, "run_build", no_build)
    assert plugin.built() == library
    assert (entry.stat().st_mtime_ns, library.stat().st_mtime_ns) == before


@pytest.mark.timeout(60)
def test_an_edit_seen_by_a_new_process_builds_a_new_entry(tmp_path: Path, cache: Path) -> None:
    source = copied("echo", tmp_path)
    code = f"from popcorn.plugin import Plugin\nprint(Plugin.dev({str(source)!r}).built())\n"
    first = run_python(code, cache)
    assert first.returncode == 0, first.stderr
    old = Path(first.stdout.strip())
    stamp = old.stat().st_mtime_ns
    (source / "echo.cpp").write_text((source / "echo.cpp").read_text() + "\n")
    second = run_python(code, cache)
    assert second.returncode == 0, second.stderr
    new = Path(second.stdout.strip())
    assert new.parent.parent != old.parent.parent
    assert old.stat().st_mtime_ns == stamp
    assert len(entries(cache, "echo")) == 2


@pytest.mark.timeout(60)
def test_an_entry_built_before_a_helper_edit_is_not_reused_after_it(
    tmp_path: Path, cache: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source = copied("echo", tmp_path)
    code = f"from popcorn.plugin import Plugin\nprint(Plugin.dev({str(source)!r}).built())\n"
    ran = run_python(code, cache)
    assert ran.returncode == 0, ran.stderr
    before = Path(ran.stdout.strip()).parent.parent
    sdk_json, build_py, sdk_py = _build_cache.helper_files()
    edited = tmp_path / "build.py"
    edited.write_text(build_py.read_text() + "\n")
    monkeypatch.setattr(_build_cache, "helper_files", lambda: (sdk_json, edited, sdk_py))
    log = tmp_path / "builds.log"
    monkeypatch.setenv("ECHO_BUILD_LOG", str(log))
    after = Plugin.dev(source).built().parent.parent
    assert after != before
    assert len(log.read_text().splitlines()) == 1
    assert len(entries(cache, "echo")) == 2


def test_an_edit_in_the_same_process_asks_for_a_restart(tmp_path: Path, cache: Path) -> None:
    source = copied("echo", tmp_path)
    plugin = Plugin.dev(source)
    plugin.built()
    (source / "echo.cpp").write_text((source / "echo.cpp").read_text() + "\n")
    with pytest.raises(ConfigError, match="restart the process") as raised:
        plugin.built()
    assert raised.value.file == str(source)


def test_editor_and_cache_files_change_neither_the_copy_nor_the_key(
    tmp_path: Path, cache: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source = copied("echo", tmp_path)
    noise = [".echo.cpp.swp", "echo.cpp~", "#echo.cpp#", "__pycache__/x.pyc", ".git/HEAD"]

    def write_noise(text: str) -> None:
        for name in noise:
            (source / name).parent.mkdir(exist_ok=True)
            (source / name).write_text(text)

    write_noise("before\n")
    plugin = Plugin.dev(source)
    library = plugin.built()
    copy = library.parent.parent / "src"
    assert not [name for name in noise if (copy / name).exists()]
    assert not (copy / "__pycache__").exists() and not (copy / ".git").exists()
    write_noise("after\n")
    (source / ".echo.hpp.swp").write_text("new\n")
    monkeypatch.setattr(_build_cache, "run_build", no_build)
    assert plugin.built() == library


@pytest.mark.parametrize("inside", ["cache", ".cache"])
def test_a_cache_root_inside_the_plugin_directory_is_refused(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, inside: str
) -> None:
    source = copied("echo", tmp_path)
    monkeypatch.setenv("POPCORN_PLUGIN_CACHE", str(source / inside))
    with pytest.raises(ConfigError, match="is inside the plugin directory") as raised:
        Plugin.dev(source).built()
    assert raised.value.key == "POPCORN_PLUGIN_CACHE"
    assert not (source / inside).exists()


def test_the_build_reads_a_copy_taken_before_it(tmp_path: Path, cache: Path) -> None:
    source = copied("echo", tmp_path)
    original = (source / "echo.cpp").read_text()
    code = f"from popcorn.plugin import Plugin\nprint(Plugin.dev({str(source)!r}).built())\n"
    ran = run_python(code, cache, {"ECHO_EDIT_DURING_BUILD": str(source / "echo.cpp")})
    assert ran.returncode == 0, ran.stderr
    entry = Path(ran.stdout.strip()).parent.parent
    assert "out + 2" in (source / "echo.cpp").read_text()
    assert (entry / "src" / "echo.cpp").read_text() == original
    pristine = copied("echo", tmp_path / "pristine")
    assert entry.name == f"echo-{_build_cache.key_of(pristine)}"


def test_an_include_outside_the_plugin_directory_fails_naming_it(
    tmp_path: Path, cache: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    other = tmp_path / "other"
    other.mkdir()
    (other / "outside.hpp").write_text("inline constexpr int outside_value = 1;\n")
    monkeypatch.setenv("OUTSIDE_INCLUDE", str(other))
    with pytest.raises(ConfigError, match="is outside the plugin directory") as raised:
        Plugin.dev(copied("outside", tmp_path)).built()
    assert str(other / "outside.hpp") in str(raised.value)


@pytest.mark.timeout(250)
def test_two_processes_building_together_build_once(tmp_path: Path, cache: Path) -> None:
    source = copied("echo", tmp_path)
    log = tmp_path / "builds.log"
    go = tmp_path / "go"
    code = f"""
        import time
        from pathlib import Path
        from popcorn.plugin import Plugin
        while not Path({str(go)!r}).exists():
            time.sleep(0.01)
        print(Plugin.dev({str(source)!r}).built())
    """
    environment = {**os.environ, "POPCORN_PLUGIN_CACHE": str(cache), "ECHO_BUILD_LOG": str(log)}
    workers = [
        subprocess.Popen(
            [sys.executable, "-c", textwrap.dedent(code)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )
        for _ in range(2)
    ]
    go.touch()
    outputs = [worker.communicate(timeout=240) for worker in workers]
    assert [worker.returncode for worker in workers] == [0, 0], outputs
    assert outputs[0][0] == outputs[1][0]
    assert len(log.read_text().splitlines()) == 1


@pytest.mark.timeout(60)
def test_a_failed_build_is_reported_once_and_cached_until_fixed(
    tmp_path: Path, cache: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source = copied("echo", tmp_path)
    good = (source / "echo.cpp").read_text()
    (source / "echo.cpp").write_text(good.replace("return out + 1;", "return out + ;"))
    with pytest.raises(ConfigError, match="the build failed") as first:
        Plugin.dev(source).built()
    assert "expected primary-expression" in str(first.value)
    code = f"from popcorn.plugin import Plugin\nprint(Plugin.dev({str(source)!r}).built())\n"
    log = tmp_path / "builds.log"
    again = run_python(code, cache, {"ECHO_BUILD_LOG": str(log)})
    assert again.returncode != 0
    assert str(first.value) in again.stderr
    assert not log.exists()
    (source / "echo.cpp").write_text(good)
    fixed = run_python(code, cache, {"ECHO_BUILD_LOG": str(log)})
    assert fixed.returncode == 0, fixed.stderr
    assert len(log.read_text().splitlines()) == 1


@pytest.mark.timeout(60)
def test_a_failed_build_fixed_in_the_same_process_builds(tmp_path: Path, cache: Path) -> None:
    source = copied("echo", tmp_path)
    good = (source / "echo.cpp").read_text()
    (source / "echo.cpp").write_text(good.replace("return out + 1;", "return out + ;"))
    plugin = Plugin.dev(source)
    with pytest.raises(ConfigError, match="the build failed"):
        plugin.built()
    assert _build_cache.known_key("echo", source) is None
    (source / "echo.cpp").write_text(good)
    library = plugin.built()
    assert library.is_file()
    assert library.parent.parent.name == f"echo-{_build_cache.known_key('echo', source)}"


def test_another_compiler_is_refused_naming_both(
    tmp_path: Path, cache: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    fake = tmp_path / "g++"
    fake.write_text(
        "#!/bin/sh\n"
        'case "$1" in -dumpfullversion) echo 15.9.9;;'
        ' *) echo "#define __VERSION__ \\"15.9.9 fake\\"";; esac\n'
    )
    fake.chmod(0o755)
    real = sdk()
    toolkit = dataclasses.replace(real, compiler=str(fake))
    monkeypatch.setattr(_build_cache, "sdk", lambda: toolkit)
    monkeypatch.setattr(_build_cache, "run_build", no_build)
    with pytest.raises(ConfigError, match="same GCC build") as raised:
        Plugin.dev(copied("echo", tmp_path)).built()
    assert "15.9.9 fake" in str(raised.value)
    assert real.compiler_ident in str(raised.value)
    assert not cache.exists() or not entries(cache, "echo")


@pytest.mark.timeout(90)
def test_five_edits_leave_at_most_two_entries(tmp_path: Path, cache: Path) -> None:
    source = copied("echo", tmp_path)
    code = f"from popcorn.plugin import Plugin\nprint(Plugin.dev({str(source)!r}).built())\n"
    for edit in range(5):
        (source / "edit.txt").write_text(f"{edit}\n")
        ran = run_python(code, cache)
        assert ran.returncode == 0, ran.stderr
    assert len(entries(cache, "echo")) == 2


@pytest.mark.timeout(60)
def test_eviction_skips_an_entry_whose_build_lock_is_held(tmp_path: Path, cache: Path) -> None:
    source = copied("echo", tmp_path)
    code = f"from popcorn.plugin import Plugin\nprint(Plugin.dev({str(source)!r}).built())\n"

    def build_edit(edit: int) -> Path:
        (source / "edit.txt").write_text(f"{edit}\n")
        ran = run_python(code, cache)
        assert ran.returncode == 0, ran.stderr
        return Path(ran.stdout.strip()).parent.parent

    held = build_edit(0)
    lock = Path(f"{held}.lock")
    descriptor = os.open(lock, os.O_RDWR)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        build_edit(1)
        build_edit(2)
        assert (held / "lib").is_dir()
        assert os.path.samestat(os.fstat(descriptor), os.stat(lock))
    finally:
        os.close(descriptor)
    build_edit(3)
    assert not held.exists()
    assert not lock.exists() and not Path(f"{held}.use").exists()
    assert len(entries(cache, "echo")) == 2


def test_eviction_removes_staging_copies_a_killed_process_left(cache: Path) -> None:
    old = cache / ".echo-copy-old"
    fresh = cache / ".echo-copy-fresh"
    other = cache / ".other-copy-old"
    for staging in (old, fresh, other):
        (staging / "src").mkdir(parents=True)
    hours_ago = time.time() - 2 * _build_cache.STAGING_AGE
    for staging in (old, other):
        os.utime(staging, (hours_ago, hours_ago))
    _build_cache.evict(cache, "echo")
    assert not old.exists()
    assert fresh.is_dir() and other.is_dir()


@pytest.mark.timeout(60)
def test_a_failed_build_keeps_no_use_lock(tmp_path: Path, cache: Path) -> None:
    source = copied("echo", tmp_path)
    good = (source / "echo.cpp").read_text()
    (source / "echo.cpp").write_text(good.replace("return out + 1;", "return out + ;"))
    for _ in range(3):
        with pytest.raises(ConfigError, match="the build failed"):
            Plugin.dev(source).built()
    (use,) = cache.glob("echo-*.use")
    descriptor = os.open(use, os.O_RDWR)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    finally:
        os.close(descriptor)


def test_the_cache_root_follows_the_variables(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("POPCORN_PLUGIN_CACHE", str(tmp_path / "explicit"))
    assert _build_cache.cache_root() == tmp_path / "explicit"
    monkeypatch.delenv("POPCORN_PLUGIN_CACHE")
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "xdg"))
    assert _build_cache.cache_root() == tmp_path / "xdg" / "popcorn" / "plugins"
    monkeypatch.delenv("XDG_CACHE_HOME")
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    assert _build_cache.cache_root() == tmp_path / "home" / ".cache" / "popcorn" / "plugins"


@pytest.mark.timeout(250)
def test_an_unset_cache_variable_builds_under_xdg_cache_home(tmp_path: Path) -> None:
    source = copied("nobind", tmp_path)
    environment = {key: value for key, value in os.environ.items() if key != "POPCORN_PLUGIN_CACHE"}
    environment["XDG_CACHE_HOME"] = str(tmp_path / "xdg")
    ran = subprocess.run(
        [
            sys.executable,
            "-c",
            f"from popcorn.plugin import Plugin\nprint(Plugin.dev({str(source)!r}).built())\n",
        ],
        capture_output=True,
        text=True,
        env=environment,
        check=False,
        timeout=240,
    )
    assert ran.returncode == 0, ran.stderr
    assert Path(ran.stdout.strip()).is_relative_to(tmp_path / "xdg" / "popcorn" / "plugins")


@pytest.mark.timeout(60)
@pytest.mark.parametrize(
    ("variable", "value"), [("CXXFLAGS", "-fabi-version=20"), ("CXX", "/nonexistent/g++")]
)
def test_compiler_and_flag_variables_do_not_reach_the_build(
    tmp_path: Path, cache: Path, monkeypatch: pytest.MonkeyPatch, variable: str, value: str
) -> None:
    monkeypatch.setenv(variable, value)
    library = Plugin.dev(copied("echo", tmp_path)).built()
    assert abi_tag(library) == _core._host_abi_tag()  # pyright: ignore[reportPrivateUsage]


def test_the_build_runs_at_most_six_jobs(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    for cpus, jobs in ((64, "6"), (2, "2"), (None, "1")):
        monkeypatch.setattr(os, "cpu_count", lambda cpus=cpus: cpus)
        command = _build_cache.build_command(tmp_path / "src", tmp_path / "build")
        assert command[command.index("-j") + 1] == jobs


def test_missing_pcons_or_ninja_is_one_config_error(
    tmp_path: Path, cache: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    def nowhere(name: str) -> None:
        return None

    monkeypatch.setattr(shutil, "which", nowhere)
    with pytest.raises(ConfigError, match="dev-mode plugins need pcons and ninja"):
        Plugin.dev(copied("echo", tmp_path)).built()


def test_a_directory_without_a_build_script_is_refused(tmp_path: Path, cache: Path) -> None:
    (tmp_path / "empty").mkdir()
    with pytest.raises(ConfigError, match=r"has no popcorn-plugin\.py"):
        Plugin.dev(tmp_path / "empty").built()


@pytest.mark.parametrize("name", ["", "two words", "1st", 3])
def test_a_plugin_name_is_an_identifier(tmp_path: Path, name: object) -> None:
    with pytest.raises(ConfigError, match="not a Python identifier"):
        Plugin.dev(tmp_path, name=name)  # pyright: ignore[reportArgumentType]


def test_plugin_construction_needs_one_of_source_and_library(tmp_path: Path) -> None:
    with pytest.raises(ConfigError, match="exactly one of source and library"):
        Plugin("echo")
    with pytest.raises(ConfigError, match="exactly one of source and library"):
        Plugin("echo", source=tmp_path, library=tmp_path / "libecho.so")
    assert Plugin("echo", library=tmp_path / "libecho.so").built() == tmp_path / "libecho.so"


def test_repeated_construction_records_one_plugin(tmp_path: Path) -> None:
    before = len(constructed())
    plugins = {Plugin.dev(tmp_path) for _ in range(1000)}
    assert len(plugins) == 1
    assert len(constructed()) == before + 1
