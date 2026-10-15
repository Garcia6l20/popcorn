from popcorn.plugin.build import PluginProject

project = PluginProject("fixture_plugin")
project.native(["fixture.cpp"])
project.bindings(["bind.cpp"])
project.wheel()
