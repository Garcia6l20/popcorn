import os

from popcorn.plugin.build import PluginProject

project = PluginProject("outside")
library = project.native(["outside.cpp"])
library.private.include_dirs.append(os.environ["OUTSIDE_INCLUDE"])
