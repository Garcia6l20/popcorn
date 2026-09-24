from collections.abc import Mapping, Sequence
from pathlib import Path

from pcons import Target, context, write_file

from popcorn_build.config import build_dir
from popcorn_build.requirements import package

_doctest_main: Target | None = None


def _doctest_with_main() -> Target:
    global _doctest_main
    if _doctest_main is None:
        project = context.current_project
        source = write_file(build_dir / "doctest" / "main.cpp", "#include <doctest/doctest.h>\n")
        _doctest_main = project.StaticLibrary(
            "doctest-main", project.default_environment, sources=[source]
        )
        _doctest_main.private.defines.append("DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN")
        _doctest_main.link(package("doctest"))
    return _doctest_main


def add_doctest(
    name: str,
    sources: Sequence[str | Path],
    deps: Sequence[Target] = (),
    group: str = "native",
    env: Mapping[str, str] | None = None,
) -> Target:
    """Build a doctest program and register one test per case under @p group, run with @p env."""
    project = context.current_project
    program = project.Program(f"test-{name}", project.default_environment, sources=list(sources))
    for dep in [*deps, package("doctest"), _doctest_with_main()]:
        program.link(dep)
    project.Test(
        f"{group}.{name}", program, discover="doctest", labels=[group], env=dict(env or {})
    )
    project.Alias(f"test-{group}", program)
    return program
