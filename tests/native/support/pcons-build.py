from pathlib import Path

from pcons import context

project = context.current_project
here = Path(__file__).resolve().parent
core = project.get_target("popcorn_core")
armv8m = project.get_target("popcorn_armv8m")
armv8m_static = project.get_target("popcorn_armv8m_static")

smoke_support = project.StaticLibrary(
    "popcorn-test-smoke", project.default_environment, sources=[here / "smoke.cpp"]
)
smoke_support.link(core)

support_objects = project.ObjectLibrary(
    "popcorn-test-support-objects",
    project.default_environment,
    sources=[here / "test_arch.cpp", here / "test_model.cpp"],
)
support_objects.link(project.get_target("popcorn_core_headers"))

test_support = project.StaticLibrary(
    "popcorn-test-support", project.default_environment, sources=[support_objects]
)
test_support.link(armv8m)

test_support_static = project.StaticLibrary(
    "popcorn-test-support-static", project.default_environment, sources=[support_objects]
)
test_support_static.link(armv8m_static)
