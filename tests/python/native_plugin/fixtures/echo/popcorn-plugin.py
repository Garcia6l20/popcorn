import os
from pathlib import Path

from popcorn.plugin.build import PluginProject

if log := os.environ.get("ECHO_BUILD_LOG"):
    with Path(log).open("a") as stream:
        stream.write(f"{os.getpid()}\n")
if edited := os.environ.get("ECHO_EDIT_DURING_BUILD"):
    Path(edited).write_text(Path(edited).read_text().replace("out + 1", "out + 2"))

project = PluginProject("echo")
project.native(["echo.cpp"])
project.bindings(["bind.cpp"])
