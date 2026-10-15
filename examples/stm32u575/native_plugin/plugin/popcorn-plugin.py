from popcorn.plugin.build import PluginProject

project = PluginProject("plugin")
project.native(["rng.cpp", "adc.cpp"])
project.bindings(["bind.cpp"])
