"""The plugin SDK digest: public popcorn headers plus the reflex headers they reach."""

from __future__ import annotations

from pathlib import Path

import pytest

from popcorn_build.sdk import build_info, main, reflex_include_dirs, sdk_digest, sdk_inputs


def write(root: Path, relative: str, text: str = "") -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


@pytest.fixture
def tree(tmp_path: Path) -> Path:
    write(tmp_path, "native/include/popcorn/core/machine.hpp", "#include <reflex/enum.hpp>\n")
    write(tmp_path, "reflex/core/include/reflex/enum.hpp", '#include "detail/names.hpp"\n')
    write(tmp_path, "reflex/core/include/reflex/detail/names.hpp", "")
    write(tmp_path, "reflex/core/include/reflex/unused.hpp", "")
    write(tmp_path, "reflex/serde/include/reflex/serde.hpp", "")
    return tmp_path


def digest(tree: Path) -> str:
    reflex = tree / "reflex"
    return sdk_digest(
        tree / "native" / "include",
        reflex,
        [reflex / "core" / "include", reflex / "serde" / "include"],
    )


@pytest.mark.parametrize(
    "changed",
    [
        "native/include/popcorn/core/machine.hpp",
        "reflex/core/include/reflex/enum.hpp",
        "reflex/core/include/reflex/detail/names.hpp",
    ],
)
def test_an_sdk_header_change_changes_the_digest(tree: Path, changed: str) -> None:
    before = digest(tree)
    (tree / changed).write_text((tree / changed).read_text() + "struct edited;\n")
    assert digest(tree) != before


@pytest.mark.parametrize(
    "unrelated",
    ["reflex/core/include/reflex/unused.hpp", "reflex/serde/include/reflex/serde.hpp"],
)
def test_a_reflex_header_no_popcorn_header_reaches_is_not_in_the_digest(
    tree: Path, unrelated: str
) -> None:
    before = digest(tree)
    (tree / unrelated).write_text("struct edited;\n")
    assert digest(tree) == before


def test_a_new_public_header_changes_the_digest(tree: Path) -> None:
    before = digest(tree)
    write(tree, "native/include/popcorn/plugin/plugin.hpp", "")
    assert digest(tree) != before


def test_build_info_spells_every_tag_field() -> None:
    text = build_info("0.1.0.dev0", "2.13.0", "release", "ab" * 32)
    assert '#define POPCORN_VERSION_STRING "0.1.0.dev0"' in text
    assert '#define POPCORN_NANOBIND_VERSION "2.13.0"' in text
    assert "#define POPCORN_NANOBIND_VERSION_PATCH 0" in text
    assert '#define POPCORN_NB_DOMAIN ""' in text
    assert '#define POPCORN_VARIANT "release"' in text
    assert f'#define POPCORN_SDK_DIGEST "{"ab" * 32}"' in text


def test_main_rewrites_build_info_only_when_its_text_changes(tree: Path) -> None:
    output = tree / "out" / "build_info.hpp"
    arguments = [
        str(output),
        "0.1.0.dev0",
        "2.13.0",
        "release",
        str(tree / "native" / "include"),
        str(tree / "reflex"),
    ]
    assert main(arguments) == 0
    first = output.read_text()
    stamp = output.stat().st_mtime_ns
    assert main(arguments) == 0
    assert output.stat().st_mtime_ns == stamp
    (tree / "reflex/core/include/reflex/enum.hpp").write_text("struct edited;\n")
    assert main(arguments) == 0
    assert output.read_text() != first


def test_the_build_edge_of_build_info_reads_every_digest_input() -> None:
    root = Path(__file__).resolve().parents[3]
    ninja = root / "build" / "build.ninja"
    if not ninja.is_file():
        pytest.skip("no build/build.ninja")
    edge = next(
        line
        for line in ninja.read_text().splitlines()
        if line.startswith("build generated/include/popcorn/plugin/build_info.hpp:")
    )
    listed = {Path(word.replace("$topdir", str(root))).resolve() for word in edge.split()[3:]}
    reflex = root / "third_party" / "reflex"
    expected = set(sdk_inputs(root / "native" / "include", reflex_include_dirs(reflex)))
    assert {path.resolve() for path in expected} <= listed
