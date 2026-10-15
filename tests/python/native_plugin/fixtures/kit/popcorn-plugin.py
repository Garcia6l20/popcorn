from popcorn.plugin.build import PluginProject

project = PluginProject("kit")
project.native(["kit.cpp"])
project.bindings(["bind.cpp"])
