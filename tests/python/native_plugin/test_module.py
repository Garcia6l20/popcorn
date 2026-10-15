"""`Plugin.module()`: the bindings module of a dev plugin, imported after the loader's checks.

Each case runs in its own interpreter: a loaded plugin is never unloaded, and its module name
`popcorn_plugins.<name>._native` belongs to the first plugin of that name in a process.
"""

from __future__ import annotations

from pathlib import Path

from .support import copied, run_python


def test_the_module_derives_from_core_and_is_imported_once(tmp_path: Path) -> None:
    source = copied("echo", tmp_path)
    ran = run_python(
        f"""
        import sys
        from popcorn import _core
        from popcorn.plugin import Plugin
        from popcorn.plugin._records import recorded
        plugin = Plugin.dev({str(source)!r})
        module = plugin.module()
        assert module.__name__ == "popcorn_plugins.echo._native", module.__name__
        assert module.EchoBase.__module__ == "popcorn_plugins.echo._native"
        assert type(module.EchoBase).__module__ == "nanobind"
        assert issubclass(module.EchoBase, _core.SpiTargetBase)
        assert sys.modules["popcorn_plugins.echo._native"] is module
        assert plugin.module() is module
        assert Plugin.dev({str(source)!r}).module() is module
        assert recorded("popcorn_plugins.echo._native") == plugin
        record = _core._load_plugin(str(plugin.built()))
        assert record.path == str(plugin.built().resolve()), record.path
        assert record.name == "echo" and record.models == ["rng"]
        print("ok")
        """,
        tmp_path / "cache",
    )
    assert ran.returncode == 0, ran.stderr
    assert ran.stdout.strip() == "ok"


def test_a_mismatched_tag_is_refused_before_its_constructors_run(tmp_path: Path) -> None:
    source = copied("mismatch", tmp_path)
    marker = tmp_path / "constructed"
    ran = run_python(
        f"""
        from popcorn import ConfigError, _core
        from popcorn.plugin import Plugin
        try:
            Plugin.dev({str(source)!r}).module()
        except ConfigError as error:
            print(error.key)
            print(error.expected)
            raise SystemExit(3)
        """,
        tmp_path / "cache",
        {"MISMATCH_MARKER": str(marker)},
    )
    assert ran.returncode == 3, ran.stderr
    key, expected = ran.stdout.splitlines()[:2]
    assert key == "abi_tag"
    assert "popcorn 0.0.0;gcc edited" in expected
    assert "popcorn 0.1.0" in expected
    assert not marker.exists()


def test_a_name_other_than_the_entry_points_is_refused_naming_both(tmp_path: Path) -> None:
    source = copied("echo", tmp_path)
    ran = run_python(
        f"""
        from popcorn import ConfigError
        from popcorn.plugin import Plugin
        try:
            Plugin.dev({str(source)!r}, name="other").module()
        except ConfigError as error:
            print(error)
            raise SystemExit(3)
        """,
        tmp_path / "cache",
    )
    assert ran.returncode == 3, ran.stderr
    assert "POPCORN_PLUGIN echo, not other" in ran.stdout


def test_a_plugin_without_bindings_is_refused_before_it_loads(tmp_path: Path) -> None:
    source = copied("nobind", tmp_path)
    ran = run_python(
        f"""
        from popcorn import ConfigError
        from popcorn.plugin import Plugin
        plugin = Plugin.dev({str(source)!r})
        try:
            plugin.module()
        except ConfigError as error:
            print(error)
        with open("/proc/self/maps") as maps:
            print("mapped" if str(plugin.built()) in maps.read() else "not mapped")
        """,
        tmp_path / "cache",
    )
    assert ran.returncode == 0, ran.stderr
    assert ran.stdout.splitlines() == ["plugin nobind has no bindings module", "not mapped"]


def test_a_second_plugin_of_one_name_is_refused(tmp_path: Path) -> None:
    first = copied("echo", tmp_path / "one")
    second = copied("echo", tmp_path / "two")
    (second / "extra.txt").write_text("another copy\n")
    ran = run_python(
        f"""
        from popcorn import ConfigError
        from popcorn.plugin import Plugin
        Plugin.dev({str(first)!r}).module()
        try:
            Plugin.dev({str(second)!r}).module()
        except ConfigError as error:
            print(error.key)
            raise SystemExit(3)
        """,
        tmp_path / "cache",
    )
    assert ran.returncode == 3, ran.stderr
    assert ran.stdout.strip() == "name"


def test_the_plugin_package_imports_without_pytest(tmp_path: Path) -> None:
    source = copied("nobind", tmp_path)
    ran = run_python(
        f"""
        import sys
        sys.modules["pytest"] = None
        import popcorn
        from popcorn.plugin import Plugin
        plugin = Plugin.dev({str(source)!r})
        print(plugin.built().name)
        print(len(popcorn.MachineDescription(part="X", images=[], plugins=[plugin]).digest))
        """,
        tmp_path / "cache",
    )
    assert ran.returncode == 0, ran.stderr
    assert ran.stdout.split() == ["libnobind.so", "64"]
