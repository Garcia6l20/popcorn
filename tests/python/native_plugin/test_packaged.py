"""Packaged plugins: a wheel built by pcons's backend, found through the `popcorn.plugins` entry
point, its model named on the board and its devices taken from `Plugin.module()`.

The `packaged` fixture project builds the wheel of `fixture_plugin`: `rng`, a counter on DR from
`0xF1C50000`, and the SPI target `FixtureTargetBase`, which answers each frame plus one. A
wheel is installed by unpacking it into a directory put on `PYTHONPATH`. Every case that loads
the plugin runs in its own interpreter, a loaded plugin is never unloaded.
"""

from __future__ import annotations

import importlib
import os
import re
import shutil
import subprocess
import sys
import textwrap
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import cast

import pytest
from elftools.elf.dynamic import DynamicSection
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import StringTableSection
from popcorn import ConfigError
from popcorn.plugin import Plugin, _build_cache, find
from popcorn.plugin._sdk import package_directory, sdk

from popcorn_build.sdk import reflex_include_dirs, sdk_digest

from .support import FIXTURES

ROOT = Path(__file__).resolve().parents[3]
SPI_ELF = ROOT / "tests" / "firmware" / "periph" / "prebuilt" / "spi_ll.elf"
PART = "STM32U575QGIx"
NAME = "fixture_plugin"
BINDINGS = f"{NAME}._native"
RNG_BASE = 0xF1C5_0000
REFUSED = "a device class of popcorn or of a bindings module Plugin.module() imported"
BACKEND = (
    "import sys\n"
    "import pcons.pyproject as backend\n"
    "print(backend.build_wheel(sys.argv[1], {'jobs': '2'}))\n"
)


@dataclass(frozen=True, slots=True)
class Installed:
    """A built fixture wheel and the directory it is unpacked into."""

    wheel: Path
    site: Path

    @property
    def package(self) -> Path:
        return self.site / NAME


def project(into: Path, dependency: str | None = None) -> Path:
    """A copy of the fixture project under @p into, its popcorn dependency @p dependency, by
    default the exact pin of this popcorn, none when @p dependency is empty."""
    target = into / "packaged"
    shutil.copytree(FIXTURES / "packaged", target)
    pyproject = target / "pyproject.toml"
    pin = f"popcorn=={sdk().version}" if dependency is None else dependency
    listed = f'["{pin}"]' if pin else "[]"
    text = re.sub(r"(?m)^dependencies = .*$", f"dependencies = {listed}", pyproject.read_text())
    pyproject.write_text(text)
    return target


def backend(
    source: Path, out: Path, pythonpath: Path | None = None
) -> subprocess.CompletedProcess[str]:
    """Run pcons's build backend over the project @p source, the wheel into @p out."""
    environment = _build_cache.build_environment()
    if pythonpath is not None:
        environment["PYTHONPATH"] = str(pythonpath)
    return subprocess.run(
        [sys.executable, "-c", BACKEND, str(out)],
        cwd=source,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
        timeout=240,
    )


def built_wheel(source: Path, out: Path, pythonpath: Path | None = None) -> Path:
    ran = backend(source, out, pythonpath)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    return out / ran.stdout.strip().splitlines()[-1]


def unpacked(wheel: Path, site: Path) -> Installed:
    with zipfile.ZipFile(wheel) as archive:
        archive.extractall(site)
    for path in (site / NAME).glob("*.so"):
        path.chmod(0o755)
    return Installed(wheel, site)


def run_installed(code: str, *sites: Path) -> subprocess.CompletedProcess[str]:
    """Run @p code in a fresh interpreter with @p sites first on `PYTHONPATH`."""
    previous = os.environ.get("PYTHONPATH")
    path = os.pathsep.join([*(str(site) for site in sites), *([previous] if previous else [])])
    return subprocess.run(
        [sys.executable, "-c", textwrap.dedent(code)],
        capture_output=True,
        text=True,
        env={**os.environ, "PYTHONPATH": path},
        check=False,
        timeout=240,
    )


def on_path(monkeypatch: pytest.MonkeyPatch, *sites: Path) -> None:
    """Put @p sites first on `sys.path` for this test, as an installation would."""
    monkeypatch.setattr(sys, "path", [*(str(site) for site in sites), *sys.path])
    importlib.invalidate_caches()


def lines(ran: subprocess.CompletedProcess[str], code: int = 0) -> list[str]:
    assert ran.returncode == code, ran.stdout + ran.stderr
    return ran.stdout.splitlines()


@pytest.fixture(scope="module", autouse=True)
def u575() -> None:
    from popcorn.stm32 import mcu
    from popcorn.stm32.description import DescriptionError

    try:
        mcu(PART)
    except DescriptionError as error:
        pytest.skip(f"no STM32U575 description: {error}")


@pytest.fixture(scope="module")
def installed(tmp_path_factory: pytest.TempPathFactory) -> Installed:
    root = tmp_path_factory.mktemp("packaged")
    wheel = built_wheel(project(root), root / "dist")
    return unpacked(wheel, root / "site")


def test_find_names_the_installed_model_on_the_board(installed: Installed) -> None:
    out = lines(
        run_installed(
            f"""
            import popcorn
            from popcorn.plugin import find
            from {NAME} import PLUGIN

            plugin = find("{NAME}")
            print(plugin == PLUGIN, plugin.bindings, plugin.source)

            def described(plugin):
                return popcorn.MachineDescription(
                    part="{PART}",
                    images=[],
                    plugins=[plugin],
                    board={{"RNG": popcorn.Periph(model=plugin.model("rng"))}},
                )

            print(described(plugin).digest == described(PLUGIN).digest)
            m = described(plugin).machine()
            print(*(m.periph.RNG.DR.read() for _ in range(3)))
            """,
            installed.site,
        )
    )
    assert out == [
        f"True {BINDINGS} None",
        "True",
        f"{RNG_BASE} {RNG_BASE + 1} {RNG_BASE + 2}",
    ]


def test_an_installed_plugin_loads_without_pcons(installed: Installed) -> None:
    out = lines(
        run_installed(
            f"""
            import sys
            sys.modules["pcons"] = None
            import popcorn
            from popcorn.plugin import find

            plugin = find("{NAME}")
            print(plugin.built() == plugin.library)
            print(plugin.module().__name__)
            m = popcorn.MachineDescription(
                part="{PART}",
                images=[],
                plugins=[plugin],
                board={{"RNG": popcorn.Periph(model=plugin.model("rng"))}},
            ).machine()
            print(m.periph.RNG.DR.read())
            """,
            installed.site,
        )
    )
    assert out == ["True", BINDINGS, str(RNG_BASE)]


def test_a_device_from_plugin_module_attaches_and_runs(installed: Installed) -> None:
    out = lines(
        run_installed(
            f"""
            import asyncio
            import pathlib
            import struct
            import popcorn
            from popcorn import ResetKind, StopReason
            from popcorn.devices import chip_select
            from popcorn.plugin import find

            plugin = find("{NAME}")

            class Echo(plugin.module().FixtureTargetBase):
                def __init__(self):
                    super().__init__()
                    self._cs_port, self._cs_pin = chip_select("PA4")

            m = popcorn.MachineDescription(
                part="{PART}",
                images=[popcorn.Elf(pathlib.Path({str(SPI_ELF)!r}))],
                plugins=[plugin],
                board={{"SPI1": popcorn.Periph(devices=[popcorn.Device(Echo)])}},
            ).machine()
            m.reset(ResetKind.POWER_ON)
            stop = asyncio.run(m.run(until="main", budget=100_000_000))
            assert stop.reason is StopReason.ADDRESS, stop
            m.reg.pc = m.sym("poll")
            stop = asyncio.run(m.run(budget=100_000_000))
            assert stop.reason is StopReason.USER, stop
            count = m.mem.u32[m.sym("log_len")]
            raw = m.mem.read(m.sym("log_buf"), 4 * count)
            print(*struct.unpack(f"<{{count}}I", raw))
            print(type(m.board.devices("SPI1")[0]).__name__)
            """,
            installed.site,
        )
    )
    assert out == [f"{0xA0} 1 1 1", "Echo"]


def test_a_device_of_a_direct_import_is_refused(installed: Installed) -> None:
    out = lines(
        run_installed(
            f"""
            import popcorn
            from popcorn import ConfigError
            import {BINDINGS} as direct
            from {NAME} import PLUGIN

            class Sub(direct.FixtureTargetBase):
                pass

            bound = (direct.FixtureTargetBase, Sub, direct.spi.NestedTargetBase)
            for cls in (*bound, direct.MadeTargetBase):
                try:
                    popcorn.MachineDescription(
                        part="{PART}",
                        images=[],
                        plugins=[PLUGIN],
                        board={{"SPI1": popcorn.Periph(devices=[popcorn.Device(cls)])}},
                    )
                except ConfigError as error:
                    print(error.key, "|", error.expected)
            try:
                PLUGIN.module()
            except ConfigError as error:
                print(error.key)
            """,
            installed.site,
        )
    )
    assert len(out) == 5, out
    modules = (BINDINGS, BINDINGS, f"{BINDINGS}.spi", BINDINGS)
    for line, module in zip(out[:4], modules, strict=True):
        key, expected = line.split(" | ")
        assert key == "board.SPI1.devices[0]"
        assert expected.startswith(REFUSED)
        assert expected.endswith(f"from {module}")
    assert out[4] == "dlopen"


def test_a_class_of_a_bindings_module_no_plugin_named_is_refused(installed: Installed) -> None:
    (path,) = installed.package.glob("_native*.so")
    out = lines(run_installed(f"""
            import importlib.util
            import sys
            import popcorn
            from popcorn import ConfigError

            spec = importlib.util.spec_from_file_location("alias._native", {str(path)!r})
            alias = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(alias)
            for cls in (alias.FixtureTargetBase, alias.spi.NestedTargetBase):
                try:
                    popcorn.MachineDescription(
                        part="{PART}",
                        images=[],
                        board={{"SPI1": popcorn.Periph(devices=[popcorn.Device(cls)])}},
                    )
                except ConfigError as error:
                    print(error.key, "|", error.expected)
            print("{NAME}" in sys.modules)
            """))
    assert out == [
        f"board.SPI1.devices[0] | {REFUSED}, not alias._native:FixtureTargetBase from"
        " alias._native",
        f"board.SPI1.devices[0] | {REFUSED}, not alias._native.spi:NestedTargetBase from"
        " alias._native.spi",
        "False",
    ]


def test_a_class_of_a_plugin_module_submodule_follows_its_plugin(installed: Installed) -> None:
    out = lines(
        run_installed(
            f"""
            import popcorn
            from popcorn import ConfigError
            from popcorn.plugin import find

            plugin = find("{NAME}")
            nested = plugin.module().spi.NestedTargetBase
            for plugins in ([], [plugin]):
                try:
                    popcorn.MachineDescription(
                        part="{PART}",
                        images=[],
                        plugins=plugins,
                        board={{"SPI1": popcorn.Periph(devices=[popcorn.Device(nested)])}},
                    )
                    print("accepted")
                except ConfigError as error:
                    print(error.expected)
            """,
            installed.site,
        )
    )
    assert out == [
        f"a device of a plugin in plugins, {BINDINGS}.spi:NestedTargetBase derives from"
        f" {BINDINGS}.spi:NestedTargetBase of plugin {NAME}",
        "accepted",
    ]


def test_a_device_of_an_undeclared_installed_plugin_is_refused(installed: Installed) -> None:
    out = lines(
        run_installed(
            f"""
            import popcorn
            from popcorn import ConfigError
            from popcorn.plugin import find

            base = find("{NAME}").module().FixtureTargetBase
            try:
                popcorn.MachineDescription(
                    part="{PART}",
                    images=[],
                    board={{"SPI1": popcorn.Periph(devices=[popcorn.Device(base)])}},
                )
            except ConfigError as error:
                print(error.key, "|", error.expected)
            """,
            installed.site,
        )
    )
    assert out == [
        f"board.SPI1.devices[0] | a device of a plugin in plugins, {BINDINGS}:FixtureTargetBase"
        f" derives from {BINDINGS}:FixtureTargetBase of plugin {NAME}"
    ]


def copy_of(installed: Installed, into: Path) -> Path:
    """A copy of the installed package under @p into, with its bindings module also in a
    subpackage `ext`, the site directory."""
    package = into / NAME
    shutil.copytree(installed.package, package)
    (bindings,) = package.glob("_native*.so")
    (package / "ext").mkdir()
    shutil.copy2(bindings, package / "ext" / bindings.name)
    return into


def appended(path: Path) -> None:
    (bindings,) = path.glob("_native*.so")
    bindings.write_bytes(bindings.read_bytes() + b"\0")


def installed_digest(site: Path, bindings: str) -> str:
    """The digest of a description declaring the plugin installed in @p site whose bindings
    module is @p bindings, in an interpreter where @p site is on `sys.path`."""
    (digest,) = lines(
        run_installed(
            f"""
            import popcorn
            from popcorn.plugin import Plugin

            plugin = Plugin.installed(
                "{NAME}", {str(site / NAME / f"lib{NAME}.so")!r}, bindings="{bindings}"
            )
            print(popcorn.MachineDescription(part="{PART}", images=[], plugins=[plugin]).digest)
            """,
            site,
        )
    )
    return digest


def test_the_installed_digest_covers_the_bindings_module(
    installed: Installed, tmp_path: Path
) -> None:
    first = copy_of(installed, tmp_path / "a")
    same = copy_of(installed, tmp_path / "b")
    edited = copy_of(installed, tmp_path / "c")
    appended(edited / NAME)
    edited_nested = copy_of(installed, tmp_path / "d")
    appended(edited_nested / NAME / "ext")
    library = (first / NAME / f"lib{NAME}.so").read_bytes()
    assert (edited / NAME / f"lib{NAME}.so").read_bytes() == library
    assert (edited_nested / NAME / f"lib{NAME}.so").read_bytes() == library
    nested = f"{NAME}.ext._native"
    assert installed_digest(first, BINDINGS) == installed_digest(same, BINDINGS)
    assert installed_digest(first, BINDINGS) != installed_digest(edited, BINDINGS)
    assert installed_digest(first, nested) != installed_digest(edited_nested, nested)
    assert installed_digest(first, nested) != installed_digest(first, BINDINGS)


def test_find_lists_the_installed_names(
    installed: Installed, monkeypatch: pytest.MonkeyPatch
) -> None:
    on_path(monkeypatch, installed.site)
    with pytest.raises(ConfigError) as raised:
        find("nope")
    assert str(raised.value).startswith(f"no installed plugin nope, installed: {NAME}")
    assert raised.value.key == "name"
    with pytest.raises(ConfigError) as raised:
        find("")
    assert raised.value.key == "name"


def write_distribution(site: Path, name: str, entry: str) -> None:
    info = site / f"{name.replace('-', '_')}-0.1.0.dist-info"
    info.mkdir(parents=True)
    (info / "METADATA").write_text(f"Metadata-Version: 2.4\nName: {name}\nVersion: 0.1.0\n")
    (info / "entry_points.txt").write_text(f"[popcorn.plugins]\n{entry}\n")


def test_two_distributions_of_one_name_are_refused_naming_both(
    installed: Installed, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    other = tmp_path / "other"
    write_distribution(other, "other-plugin", f"{NAME} = {NAME}:PLUGIN")
    on_path(monkeypatch, other, installed.site)
    with pytest.raises(ConfigError) as raised:
        find(NAME)
    assert str(raised.value).startswith(
        f"installed plugin {NAME} is provided by fixture_plugin and other-plugin"
    )


def test_one_distribution_declaring_a_name_twice_is_refused(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    site = tmp_path / "site"
    write_distribution(site, "twice", "doubled = twice_plugin:A\ndoubled = twice_plugin:B")
    on_path(monkeypatch, site)
    with pytest.raises(ConfigError) as raised:
        find("doubled")
    assert str(raised.value).startswith("installed plugin doubled is declared twice by twice")


def test_find_refuses_an_entry_point_that_does_not_load(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    site = tmp_path / "site"
    write_distribution(
        site, "broken", "gone = broken_absent_module:PLUGIN\nunset = broken_plugin:PLUGIN"
    )
    (site / "broken_plugin.py").write_text("")
    on_path(monkeypatch, site)
    for name, value in (("gone", "broken_absent_module:PLUGIN"), ("unset", "broken_plugin:PLUGIN")):
        with pytest.raises(ConfigError) as raised:
            find(name)
        assert str(raised.value).startswith(f"installed plugin {name}: {value} does not load")
        assert raised.value.key == "name"


def test_find_refuses_an_entry_point_that_is_not_its_plugin(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    site = tmp_path / "site"
    write_distribution(site, "bogus", "bogus = bogus_plugin:PLUGIN\nrenamed = bogus_plugin:OTHER")
    (site / "bogus_plugin.py").write_text(
        "from popcorn.plugin import Plugin\n"
        "PLUGIN = 3\n"
        "OTHER = Plugin.installed('other', '/nonexistent/libother.so')\n"
    )
    on_path(monkeypatch, site)
    for name, value in (("bogus", "3"), ("renamed", "Plugin(name='other'")):
        with pytest.raises(ConfigError) as raised:
            find(name)
        assert str(raised.value).startswith(f"installed plugin {name}: bogus_plugin:")
        assert value in str(raised.value)
        assert raised.value.expected == f"a popcorn.plugin.Plugin named {name}"


@pytest.mark.parametrize(
    "dependency",
    [
        "popcorn>=0.1",
        "popcorn==0.0.9",
        "popcorn==VERSION; python_version > '3'",
        "",
    ],
)
def test_a_wheel_without_the_exact_pin_is_refused_writing_nothing(
    tmp_path: Path, dependency: str
) -> None:
    version = sdk().version
    source = project(tmp_path, dependency.replace("VERSION", version))
    out = tmp_path / "dist"
    ran = backend(source, out)
    assert ran.returncode != 0
    assert f"a plugin wheel pins popcorn=={version} exactly" in ran.stdout + ran.stderr
    assert not list(out.glob("*.whl"))
    assert not (source / "build" / "build.ninja").exists()


def test_a_wheel_without_bindings_is_refused_before_any_compile(tmp_path: Path) -> None:
    source = project(tmp_path)
    script = source / "pcons-build.py"
    text = script.read_text()
    assert 'project.bindings(["bind.cpp"])\n' in text
    script.write_text(text.replace('project.bindings(["bind.cpp"])\n', ""))
    out = tmp_path / "dist"
    ran = backend(source, out)
    assert ran.returncode != 0
    assert "a plugin wheel needs bindings() before wheel()" in ran.stdout + ran.stderr
    assert not list(out.glob("*.whl"))
    assert not (source / "build" / "build.ninja").exists()
    assert not list(source.rglob("*.o"))


def scripted(source: Path, calls: str, wheel: bool) -> str:
    """What the `PluginProject` calls @p calls raise in a script run in @p source, in a wheel
    build when @p wheel, `ok` when they raise nothing."""
    code = "\n".join(
        [
            "import os, sys",
            "import popcorn.plugin.build as build",
            "from popcorn import ConfigError",
            f"build.get_var = lambda name, default: {wheel!r}",
            f"project = build.PluginProject({NAME!r})",
            "try:",
            *(f"    {call}" for call in calls.split(";")),
            "    print('ok')",
            "except ConfigError as error:",
            "    print('ConfigError', error.key)",
            "except ValueError as error:",
            "    print('ValueError', error)",
        ]
    )
    ran = subprocess.run(
        [sys.executable, "-c", code],
        cwd=source,
        capture_output=True,
        text=True,
        env={**os.environ, "PCONS_SOURCE_DIR": str(source)},
        check=False,
        timeout=240,
    )
    (line,) = lines(ran)
    return line


NATIVE = "project.native(['fixture.cpp'])"
BIND = "project.bindings(['bind.cpp'])"


@pytest.mark.parametrize("wheel", [False, True])
def test_plugin_project_calls_come_in_order(tmp_path: Path, wheel: bool) -> None:
    source = project(tmp_path)
    order = f"ValueError plugin {NAME}:"
    assert scripted(source, "project.wheel()", wheel) == f"{order} native() comes before wheel()"
    twice = f"{NATIVE};{BIND};project.wheel();project.wheel()"
    assert scripted(source, twice, wheel) == f"{order} wheel() is called once"
    late = f"{NATIVE};project.wheel();{BIND}"
    expected = f"{order} bindings() comes before wheel()" if not wheel else "ConfigError bindings"
    assert scripted(source, late, wheel) == expected
    assert scripted(source, f"{NATIVE};{BIND};project.wheel()", wheel) == "ok"


def test_a_wheel_without_its_package_or_a_readable_pyproject_is_refused(tmp_path: Path) -> None:
    calls = f"{NATIVE};{BIND};project.wheel()"
    missing = project(tmp_path / "missing")
    shutil.rmtree(missing / NAME)
    assert scripted(missing, calls, True) == "ConfigError package"
    assert scripted(missing, calls, False) == "ok"
    broken = project(tmp_path / "broken")
    (broken / "pyproject.toml").write_text("[project\n")
    assert scripted(broken, calls, True) == "ConfigError pyproject"
    absent = project(tmp_path / "absent")
    (absent / "pyproject.toml").unlink()
    assert scripted(absent, calls, True) == "ConfigError pyproject"


def runpath(path: Path) -> list[str]:
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        dynamic = elf.get_section_by_name(".dynamic")
        assert isinstance(dynamic, DynamicSection)
        strings = elf.get_section_by_name(".dynstr")
        assert isinstance(strings, StringTableSection)
        tags = {tag.entry.d_tag: tag.entry.d_val for tag in dynamic.iter_tags()}
        assert "DT_RPATH" not in tags
        return strings.get_string(tags["DT_RUNPATH"]).split(":")


def test_the_installed_files_carry_only_origin_relative_rpaths(installed: Installed) -> None:
    (bindings,) = installed.package.glob("_native*.so")
    assert runpath(installed.package / f"lib{NAME}.so") == ["$ORIGIN/../popcorn"]
    assert runpath(bindings) == ["$ORIGIN", "$ORIGIN/../popcorn"]


def abi_tag(library: Path) -> str:
    with library.open("rb") as stream:
        section = ELFFile(stream).get_section_by_name(".popcorn_abi")
        assert section is not None
        return section.data().rstrip(b"\0").decode()


def edited_header(include: Path) -> Path:
    header = include / "popcorn" / "periph" / "model.hpp"
    data = bytearray(header.read_bytes())
    at = data.index(b"@brief") + 1
    data[at] = ord("B")
    header.write_bytes(bytes(data))
    return header


def overlay(tmp_path: Path, digest: str) -> Path:
    """A popcorn package beside this one whose SDK has one byte of `periph/model.hpp` changed
    and `digest` in its `build_info.hpp`, the root to put on `PYTHONPATH`."""
    staged = package_directory()
    root = tmp_path / "overlay"
    target = root / "popcorn"
    target.mkdir(parents=True)
    for entry in staged.iterdir():
        if entry.name in ("plugin", "sdk"):
            shutil.copytree(
                entry, target / entry.name, ignore=shutil.ignore_patterns("__pycache__")
            )
        else:
            (target / entry.name).symlink_to(entry)
    include = target / "sdk" / "include"
    edited_header(include)
    info = include / "popcorn" / "plugin" / "build_info.hpp"
    text = re.sub(
        r'POPCORN_SDK_DIGEST "[0-9a-f]+"', f'POPCORN_SDK_DIGEST "{digest}"', info.read_text()
    )
    info.write_text(text)
    return root


def test_a_wheel_built_against_other_sdk_headers_is_refused(
    installed: Installed, tmp_path: Path
) -> None:
    host = abi_tag(installed.package / f"lib{NAME}.so")
    reflex = ROOT / "third_party" / "reflex"
    headers = tmp_path / "headers"
    shutil.copytree(ROOT / "native" / "include", headers)
    original = sdk_digest(headers, reflex, reflex_include_dirs(reflex))
    assert host.endswith(f";sdk {original}")
    edited_header(headers)
    digest = sdk_digest(headers, reflex, reflex_include_dirs(reflex))
    assert digest != original
    root = overlay(tmp_path, digest)
    wheel = built_wheel(project(tmp_path), tmp_path / "dist", root)
    site = unpacked(wheel, tmp_path / "site")
    tag = abi_tag(site.package / f"lib{NAME}.so")
    assert tag == host.replace(original, digest)
    out = lines(
        run_installed(
            f"""
            from popcorn import ConfigError
            from popcorn.plugin import find

            try:
                find("{NAME}").module()
            except ConfigError as error:
                print(error.key)
                print(error.expected)
                raise SystemExit(3)
            """,
            site.site,
        ),
        3,
    )
    assert out[0] == "abi_tag"
    assert tag in out[1] and host in out[1]


def test_an_installed_plugin_keeps_its_library_absolute(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.chdir(tmp_path)
    assert Plugin.installed("x", "libx.so").library == tmp_path / "libx.so"


def test_an_installed_plugin_is_checked_at_the_call(tmp_path: Path) -> None:
    library = tmp_path / "libx.so"
    plugin = Plugin.installed("x", str(library), bindings="x._native")
    assert (plugin.library, plugin.bindings, plugin.source) == (library, "x._native", None)
    assert Plugin.installed("x", library).bindings is None
    for bindings in ("x..native", "x.1st", "", 3):
        with pytest.raises(ConfigError) as raised:
            Plugin.installed("x", library, bindings=cast(str, bindings))
        assert raised.value.key == "bindings"
    for bindings in ("popcorn", "popcorn.devices._native", "popcorn_plugins.x._native"):
        with pytest.raises(ConfigError) as raised:
            Plugin.installed("x", library, bindings=bindings)
        assert raised.value.key == "bindings"
    with pytest.raises(ConfigError) as raised:
        Plugin("x", source=tmp_path, bindings="x._native")
    assert raised.value.key == "bindings"
    with pytest.raises(ConfigError) as raised:
        Plugin.installed("x", library).module()
    assert raised.value.key == "bindings"


def test_a_bindings_name_of_a_python_module_is_a_config_error(installed: Installed) -> None:
    out = lines(
        run_installed(
            f"""
            from popcorn import ConfigError
            from popcorn.plugin import Plugin
            from {NAME} import PLUGIN

            try:
                Plugin.installed("{NAME}", PLUGIN.library, bindings="{NAME}").module()
            except ConfigError as error:
                print(error.key, "|", error)
            """,
            installed.site,
        )
    )
    assert len(out) == 1, out
    key, message = out[0].split(" | ")
    assert key == "bindings"
    assert message.startswith(f"plugin {NAME}: bindings module {NAME} is not an extension module")


def test_a_bindings_module_that_does_not_import_is_a_config_error(installed: Installed) -> None:
    out = lines(
        run_installed(
            f"""
            from popcorn import ConfigError
            from popcorn.plugin import Plugin
            from {NAME} import PLUGIN

            try:
                Plugin.installed("{NAME}", PLUGIN.library, bindings="{NAME}._absent").module()
            except ConfigError as error:
                print(error.key)
            """,
            installed.site,
        )
    )
    assert out == ["bindings"]
