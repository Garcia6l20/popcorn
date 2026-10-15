from collections.abc import Mapping, Sequence
from pathlib import Path

from pcons import Target, context, write_file

from popcorn_build.config import build_dir, project_dir
from popcorn_build.core_links import both_flavours, reached
from popcorn_build.requirements import package

_doctest_main: Target | None = None

DEFAULT_CASE_TIMEOUT = 300.0
"""Seconds a native test case may run before it is killed and failed, unless its suite sets
another limit. The longest case under the full gate takes about 15 s."""


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


def alias_bare_name(target: Target) -> None:
    """Make `pcons build <name>` reach @p target when a subdirectory script declared it.

    Ninja knows a target declared under a subdirectory only by its path-qualified output, such as
    tests/native/core/test-core-types. Root targets already answer to their bare name.
    """
    if target.source_dir != project_dir:
        context.current_project.Alias(target.name, target)


def popcorn_libraries(*areas: str, static: bool = False) -> list[Target]:
    """The popcorn libraries of @p areas, such as "stm32" and "core", in one flavour.

    @p static picks the static twins, for programs that reach internals. A program links one
    flavour only, check_core_links refuses a mix.
    """
    suffix = "_static" if static else ""
    return [context.current_project.get_target(f"popcorn_{area}{suffix}") for area in areas]


def add_doctest(
    name: str,
    sources: Sequence[str | Path],
    deps: Sequence[Target] = (),
    group: str = "native",
    env: Mapping[str, str] | None = None,
    register: bool = True,
    timeout: float | None = DEFAULT_CASE_TIMEOUT,
) -> Target:
    """Build a doctest program and register one test per case under @p group, run with @p env.

    With @p register false the program is built but no test is registered, for tests whose
    inputs this machine lacks. @p timeout, in seconds, kills a case that runs longer and fails
    it, so a deadlock fails the gate instead of hanging it. None for no limit.
    """
    project = context.current_project
    program = project.Program(f"test-{name}", project.default_environment, sources=list(sources))
    for dep in [*deps, package("doctest"), _doctest_with_main()]:
        program.link(dep)
    if register:
        project.Test(
            f"{group}.{name}",
            program,
            discover="doctest",
            labels=[group],
            env=dict(env or {}),
            timeout=timeout,
        )
    project.Alias(f"test-{group}", program)
    alias_bare_name(program)
    return program


def add_header_check(
    name: str, include_root: Path, deps: Sequence[Target] = (), exclude: Sequence[str] = ()
) -> Target:
    """Compile every header under @p include_root alone in its own TU, and all of them in one.

    The TUs are generated into the build directory, one per header, so a new header is checked
    without editing a list. The TU with every header carries the program's only test case.
    @p exclude lists headers, relative to @p include_root, that need more than @p deps, each
    checked by its own TU elsewhere.
    """
    headers = sorted(
        relative
        for relative in (
            path.relative_to(include_root).as_posix() for path in include_root.rglob("*.hpp")
        )
        if relative not in exclude
    )
    generated = build_dir / "header-check" / name
    sources: list[Path] = []
    for header in headers:
        stem = header.removesuffix(".hpp").replace("/", "_")
        sources.append(write_file(generated / f"{stem}.cpp", f"#include <{header}>\n"))
    together = "".join(f"#include <{header}>\n" for header in headers)
    together += (
        "\n#include <doctest/doctest.h>\n\n"
        'TEST_CASE("every interface header compiles in one translation unit")\n'
        "{\n  CHECK_FALSE(popcorn::core::version().empty());\n}\n"
    )
    sources.append(write_file(generated / "all.cpp", together))
    return add_doctest(name, sources, deps)


def check_core_links(shared: Sequence[Target], static: Sequence[Target]) -> None:
    """Refuse a target reaching both popcorn library flavours, give programs on @p shared an rpath.

    @p shared lists every popcorn shared library, @p static every static twin. A program or
    library reaches libraries of one flavour only: one of each puts two cores, two registries
    and two Unicorn copies in one process, also when the shared one comes in through another
    library. Programs reaching a shared library run from the build tree, found through
    build/popcorn.
    """
    targets = context.current_project.targets
    refused = both_flavours(targets, shared, static)
    if refused:
        raise SystemExit(
            f"popcorn: {', '.join(refused)} link both shared and static popcorn libraries"
        )
    shared_ids = {id(item) for item in shared}
    for target in targets:
        if target.target_type == "program" and any(
            id(item) in shared_ids for item in reached(target)
        ):
            target.private.link_flags.append(f"-Wl,-rpath,{build_dir / 'popcorn'}")
