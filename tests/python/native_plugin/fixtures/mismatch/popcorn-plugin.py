from popcorn.plugin.build import PluginProject

project = PluginProject("mismatch")
project.native(["mismatch.cpp"])
project.bindings(["bind.cpp"])
