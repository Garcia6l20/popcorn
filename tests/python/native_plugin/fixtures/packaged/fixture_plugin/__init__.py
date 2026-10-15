from pathlib import Path

from popcorn.plugin import Plugin

PLUGIN = Plugin.installed(
    "fixture_plugin",
    Path(__file__).parent / "libfixture_plugin.so",
    bindings="fixture_plugin._native",
)
