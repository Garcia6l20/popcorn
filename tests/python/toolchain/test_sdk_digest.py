"""The plugin SDK digest: public popcorn headers plus the reflex headers they reach."""

from __future__ import annotations

from pathlib import Path

import pytest

from popcorn_build.sdk import (
    build_info,
    main,
    normalized_text,
    reflex_include_dirs,
    sdk_digest,
    sdk_inputs,
)


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


MACHINE = "native/include/popcorn/core/machine.hpp"

HEADER = r"""#include <reflex/enum.hpp>
#define LIMIT 4
#define LABEL "a  b"
/** @brief A machine. */
class machine {
public:
    int run(int steps = 3) { return steps + LIMIT; }
    const char *text() const { return "x // y /* z */  w"; }
    const char *raw() const { return R"x(// not /* a comment )" )x"; }
    char quote() const { return '"'; }
private:
    int state_ = 0;
};
"""


def digest_of(tree: Path, text: str) -> str:
    (tree / MACHINE).write_text(text)
    return digest(tree)


LAYOUT_EDITS = {
    "line-comment": ("class machine", "// a note\nclass machine"),
    "inserted-comment": ("int run", "// trailing note\n    int run"),
    "block-and-trailing": ("int state_ = 0;", "/* block */ int state_ = 0; // note"),
    "doxygen": ("/** @brief A machine. */", "/**\n * @brief Other.\n * @param x y\n */"),
    "reindent": ("    ", "\t"),
    "reflow": ("{ return steps + LIMIT; }", "{\n  return steps\n      + LIMIT;\n}\n\n\n"),
    "comment-in-directive": ("#define LIMIT 4", "#define LIMIT /* four */ 4 // value"),
    "crlf": ("\n", "\r\n"),
}


@pytest.mark.parametrize("name", LAYOUT_EDITS)
def test_comments_and_layout_keep_the_digest(tree: Path, name: str) -> None:
    old, new = LAYOUT_EDITS[name]
    assert digest_of(tree, HEADER.replace(old, new)) == digest_of(tree, HEADER)


@pytest.mark.parametrize(
    "old, new",
    [
        ("steps + LIMIT", "steps - LIMIT"),
        ("steps + LIMIT", "steps+LIMIT"),
        ("int state_ = 0;", "long state_ = 0;"),
        ("int state_ = 0;", "int state_ = 0; int extra_;"),
        ("steps = 3", "steps = 4"),
        ("#define LIMIT 4", "#define LIMIT 5"),
        ("#define LIMIT 4", "#define LIMIT(x) 4"),
        ("#define LIMIT(x) 4", "#define LIMIT (x) 4"),
        ('"a  b"', '"c  b"'),
        ('"x // y /* z */  w"', '"x // y /* q */  w"'),
        ("// not /* a comment", "// not /* a comment!"),
        ("'\"'", "'a'"),
        ("class machine", "struct machine"),
    ],
)
def test_a_token_change_changes_the_digest(tree: Path, old: str, new: str) -> None:
    text = (
        HEADER.replace("#define LIMIT 4", "#define LIMIT(x) 4") if old.endswith("(x) 4") else HEADER
    )
    assert old in text
    assert digest_of(tree, text.replace(old, new)) != digest_of(tree, text)


def test_comment_markers_in_literals_are_not_comments(tmp_path: Path) -> None:
    header = write(tmp_path, "h.hpp", HEADER)
    text = normalized_text(header)
    assert '"x // y /* z */ w"' in text
    assert 'R"x(// not /* a comment )" )x"' in text
    assert "'\"'" in text
    header.write_text("/* not a comment */ int x; // nor this\n")
    assert normalized_text(header) == "int x;"


def test_a_directive_keeps_its_line_and_other_whitespace_is_a_space(tmp_path: Path) -> None:
    header = write(tmp_path, "h.hpp", "#define F(x) x\r\n\t#define   G   (x)\n\nint\n a;\n")
    assert normalized_text(header) == "#define F(x) x\n#define G (x)\nint a;"


def test_a_continued_directive_is_one_line(tmp_path: Path) -> None:
    header = write(tmp_path, "h.hpp", "#define F(x) \\\n    x + 1\nint a;\n")
    assert normalized_text(header) == "#define F(x) \\ x + 1\nint a;"


DIRECTIVE_PAIRS = {
    "body-vs-next-line": ("#define A 1\nB\n", "#define A 1 B\n"),
    "empty-macro-then-token": ("#define A (x)\n", "#define A\n(x)\n"),
    "function-vs-object-like": ("#define F(x) x\n", "#define F (x) x\n"),
    "if-body": ("#if X\nint a;\n#endif\n", "#if X int a;\n#endif\n"),
}


@pytest.mark.parametrize("name", DIRECTIVE_PAIRS)
def test_a_directive_end_changes_the_digest(tree: Path, name: str) -> None:
    first, second = DIRECTIVE_PAIRS[name]
    assert digest_of(tree, first) != digest_of(tree, second)


def test_a_reflow_around_directives_keeps_the_digest(tree: Path) -> None:
    compact = "#define A 1\n#define F(x) (x)\nint a = F(A);\nint b;\n"
    reflowed = "  #define   A   1\n\n#define F(x)\t(x)\nint a =\n    F(A);\n\n\nint\nb;\n"
    assert digest_of(tree, reflowed) == digest_of(tree, compact)


def test_a_no_break_space_in_a_literal_changes_the_digest(tree: Path) -> None:
    assert digest_of(tree, 'const char *s = "a b";\n') != digest_of(
        tree, 'const char *s = "a\u00a0b";\n'
    )
    assert digest_of(tree, 'const char *s = "a\u2003b";\n') != digest_of(
        tree, 'const char *s = "a\u00a0b";\n'
    )


def test_a_non_utf8_byte_is_a_runtime_error(tmp_path: Path) -> None:
    header = tmp_path / "h.hpp"
    header.write_bytes(b'const char *s = "\xff";\n')
    with pytest.raises(RuntimeError, match="non-UTF-8"):
        normalized_text(header)


def test_a_missing_compiler_is_an_error(tree: Path) -> None:
    reflex = tree / "reflex"
    with pytest.raises(RuntimeError, match="no-such-compiler"):
        sdk_digest(tree / "native" / "include", reflex, [], "no-such-compiler")


def test_a_failing_compiler_is_an_error(tree: Path) -> None:
    reflex = tree / "reflex"
    with pytest.raises(RuntimeError, match="exited with"):
        sdk_digest(tree / "native" / "include", reflex, [], "false")
