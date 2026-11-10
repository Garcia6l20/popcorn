import shutil
import subprocess
from pathlib import Path

import pytest

from popcorn_build import limits_docs
from popcorn_build.support_pages import LIMIT_ROOTS, ROOT

BLOCK = """/**
 * @brief Thing of the STM32U5, RM0456 section 9. It does more.
 *
 * @limits IDENT TITLE
 *
 * @group u5,h5 deviation "Differs from RM0456"
 * - First bullet, RM0456 9.1, inferred.
 *   Second line of it.
 * - Second bullet, ES0499 2.1.
 *
 * @group all rule
 * - Plain rule.
 */
class thing;
"""


def block(ident: str, title: str) -> str:
    return BLOCK.replace("IDENT", ident).replace("TITLE", title)


def write(
    root: Path, name: str, ident: str = "thing", title: str = "Thing", text: str = ""
) -> None:
    root.mkdir(exist_ok=True)
    (root / name).write_text(text or block(ident, title))


def test_a_block_becomes_a_page_with_a_section_per_group(tmp_path: Path) -> None:
    write(tmp_path, "thing.cpp")
    page = limits_docs.read_pages([tmp_path])["thing"]
    assert (page.title, page.lead) == ("Thing", "Thing of the STM32U5, RM0456 section 9.")
    assert [(g.scopes, g.kind, g.heading) for g in page.groups] == [
        (("u5", "h5"), "deviation", "Differs from RM0456"),
        ((), "rule", "Every family: Rules"),
    ]
    assert page.groups[0].body.splitlines() == [
        "- First bullet, RM0456 9.1, inferred.",
        "  Second line of it.",
        "- Second bullet, ES0499 2.1.",
    ]
    assert page.families == ["STM32U5", "STM32H5", "STM32L1", "STM32L4"]
    assert page.inferred == 1
    assert page.references == ["RM0456", "ES0499"]
    text = limits_docs.render_page(page, lambda path: f"url/{path.name}")
    assert text.startswith("# Thing\n\nThing of the STM32U5, RM0456 section 9.\n\nSources: ")
    assert "[thing.cpp](url/thing.cpp)" in text
    assert "\n## Differs from RM0456\n\n- First bullet" in text
    assert "References: RM0456, ES0499.\n" in text


def test_a_default_heading_names_the_families_and_the_kind(tmp_path: Path) -> None:
    text = block("a", "A").replace('u5,h5 deviation "Differs from RM0456"', "l1,l4 note")
    write(tmp_path, "a.cpp", text=text)
    assert limits_docs.read_pages([tmp_path])["a"].groups[0].heading == "STM32L1, STM32L4: Notes"


def test_blocks_of_one_id_merge_in_file_name_order(tmp_path: Path) -> None:
    write(tmp_path, "b.cpp", "x", "X")
    other = (
        block("x", "")
        .replace("Second bullet", "Third bullet")
        .replace("Differs from RM0456", "Differs in a.hpp")
        .replace("@group all rule", "@group all note")
    )
    write(tmp_path, "a.hpp", text=other)
    page = limits_docs.read_pages([tmp_path])["x"]
    assert [path.name for path in page.sources] == ["a.hpp", "b.cpp"]
    assert [g.file.name for g in page.groups] == ["a.hpp", "a.hpp", "b.cpp", "b.cpp"]
    assert page.title == "X"


def test_two_roots_are_scanned(tmp_path: Path) -> None:
    write(tmp_path / "one", "a.cpp", "a", "A")
    write(tmp_path / "two", "b.cpp", "b", "B")
    assert sorted(limits_docs.read_pages([tmp_path / "one", tmp_path / "two"])) == ["a", "b"]


def test_a_block_with_no_group_declares_a_source(tmp_path: Path) -> None:
    write(tmp_path, "a.cpp", "x", "X")
    declaration = "/**\n * @brief Other.\n *\n * @limits x\n */\nclass other;\n"
    write(tmp_path, "b.cpp", text=declaration)
    page = limits_docs.read_pages([tmp_path])["x"]
    assert [path.name for path in page.sources] == ["a.cpp", "b.cpp"]
    assert len(page.groups) == 2


def test_a_tag_inside_a_code_span_starts_no_section(tmp_path: Path) -> None:
    text = (
        "/**\n * @limits t T\n *\n * @group all rule\n"
        " * - Write `@group u5 rule` in a block.\n *   See `@limits id` too.\n */\n"
    )
    write(tmp_path, "t.cpp", text=text)
    group = limits_docs.read_pages([tmp_path])["t"].groups[0]
    assert group.body == "- Write `@group u5 rule` in a block.\n  See `@limits id` too."


def test_a_comment_without_the_tag_is_ignored(tmp_path: Path) -> None:
    write(tmp_path, "t.cpp", text="/**\n * @brief Mentions limits in prose.\n */\nint x;\n")
    assert limits_docs.read_pages([tmp_path]) == {}


def test_a_file_that_is_not_a_source_is_ignored(tmp_path: Path) -> None:
    write(tmp_path, "notes.md", text=block("n", "N"))
    assert limits_docs.read_pages([tmp_path]) == {}


def test_the_table_has_a_row_per_page_and_a_total(tmp_path: Path) -> None:
    write(tmp_path, "thing.cpp")
    table = limits_docs.render_table(limits_docs.read_pages([tmp_path]), "p/")
    assert (
        "| [Thing](p/thing.md) | STM32U5, STM32H5, STM32L1, STM32L4 | 2 | 1 | RM0456, ES0499 |"
        in table
    )
    assert table.splitlines()[-1] == "| **1 IPs** | | 2 | 1 | | |"


def broken(text: str) -> str:
    return "/**\n * @limits t T\n *\n" + text + " */\n"


@pytest.mark.parametrize(
    ("text", "message"),
    [
        (broken(" * @group all rulez\n * - x\n"), "t.cpp:4: unknown kind 'rulez'"),
        (broken(" * @group u9 rule\n * - x\n"), "t.cpp:4: unknown scope 'u9'"),
        (broken(" * @group all rule\n"), "t.cpp:4: @group has an empty body"),
        (broken(" * @group all\n * - x\n"), "t.cpp:4: @group needs"),
        (broken(" * @groups all rule\n * - x\n"), "t.cpp:4: unknown tag @groups"),
        (broken(" * @grp all rule\n * - x\n"), "t.cpp:4: unknown tag @grp"),
        (broken(" * @Group all rule\n * - x\n"), "t.cpp:4: unknown tag @Group"),
        (
            broken(" * @group all rule\n * - x\n * @Group l1 rule\n * - y\n"),
            "t.cpp:6: tag @Group inside a group body",
        ),
        (broken(" * @group all rule\n * - x\n * @see foo\n"), "t.cpp:6: tag @see inside"),
        (broken(" * Prose.\n * @group all rule\n * - x\n"), "t.cpp:4: text between @limits"),
        (
            "/**\n * @lmits t T\n * @group all rule\n * - x\n */\n",
            "t.cpp:2: unknown tag @lmits",
        ),
        ("/**\n * @brief Thing.\n * @lmits t T\n */\n", "t.cpp:3: unknown tag @lmits"),
        ("/**\n * @Limits t T\n */\n", "t.cpp:2: unknown tag @Limits"),
        (
            broken(" * @group u5,u5 rule\n * - x\n"),
            "t.cpp:4: a scope repeats",
        ),
        (
            broken(" * @group u5 rule\n * - x\n * @group u5 rule\n * - y\n").replace(
                "* @group u5 rule\n * - y", "* @group u5 rule\n * - y"
            ),
            "t.cpp:6: two groups of t share",
        ),
        (broken(' * @group all rule "never closed\n * - x\n'), "t.cpp:4: @group heading has no"),
        (broken(" * @limitz t\n"), "t.cpp:4: unknown tag @limitz"),
        (broken(" * @limits u\n"), "t.cpp:4: a block holds one @limits"),
        (
            "/**\n * @group all rule\n * - x\n * @limits t T\n */\n",
            "t.cpp:2: @group before @limits",
        ),
        ("/**\n * @limits Bad-Id T\n */\n", "t.cpp:2: @limits id 'Bad-Id'"),
        ("/**\n * @limits t\n * @group all rule\n * - x\n */\n", "t.cpp: @limits t has no title"),
    ],
)
def test_a_malformed_block_names_the_file_and_line(tmp_path: Path, text: str, message: str) -> None:
    write(tmp_path, "t.cpp", text=text)
    with pytest.raises(ValueError, match=r"t\.cpp") as caught:
        limits_docs.read_pages([tmp_path])
    assert message in str(caught.value)


def test_two_titles_for_one_id_fail_at_the_second(tmp_path: Path) -> None:
    write(tmp_path, "a.cpp", "x", "X")
    write(tmp_path, "b.cpp", "x", "Y")
    with pytest.raises(ValueError, match=r"b.cpp:\d+: @limits x has a title already"):
        limits_docs.read_pages([tmp_path])


def test_the_real_tree_reads_into_titled_pages() -> None:
    pages = limits_docs.read_pages(LIMIT_ROOTS)
    assert {"crc", "iwdg"} <= set(pages)
    for ident, page in pages.items():
        assert page.id == ident
        assert page.title
        assert page.groups


def test_the_pilot_pages_keep_their_marks_and_references() -> None:
    pages = limits_docs.read_pages(LIMIT_ROOTS)
    assert (pages["crc"].inferred, len(pages["crc"].groups)) == (1, 6)
    assert (pages["iwdg"].inferred, len(pages["iwdg"].groups)) == (5, 6)
    assert [g.scopes for g in pages["crc"].groups] == [
        (),
        ("u5", "h5", "l4"),
        ("u5", "h5", "l4"),
        (),
        ("l1",),
        ("l4",),
    ]
    assert [g.scopes for g in pages["iwdg"].groups] == [
        (),
        ("u5", "h5", "l1"),
        ("l1",),
        (),
        ("l4",),
        (),
    ]
    assert pages["crc"].references == ["RM0456", "RM0351", "RM0038"]
    assert pages["iwdg"].references == ["RM0456", "RM0038", "RM0351"]


def test_two_blocks_of_one_id_in_one_file_keep_source_order(tmp_path: Path) -> None:
    second = (
        block("thing", "")
        .replace("Differs from RM0456", "Second block")
        .replace("@group all rule", "@group l4 note")
    )
    write(tmp_path, "thing.cpp", text=block("thing", "Thing") + "\n" + second)
    page = limits_docs.read_pages([tmp_path])["thing"]
    assert [g.heading for g in page.groups] == [
        "Differs from RM0456",
        "Every family: Rules",
        "Second block",
        "STM32L4: Notes",
    ]
    assert [path.name for path in page.sources] == ["thing.cpp"]
    assert page.groups[2].line > page.groups[1].line


def test_a_quoted_heading_continues_over_the_next_lines(tmp_path: Path) -> None:
    text = block("w", "W").replace(
        '"Differs from RM0456"',
        '"On the STM32L47x/L48x, RM0351 section 18, ADC1 run the\n * STM32H5 model\n * and more"',
    )
    write(tmp_path, "w.cpp", text=text)
    group = limits_docs.read_pages([tmp_path])["w"].groups[0]
    assert group.heading == (
        "On the STM32L47x/L48x, RM0351 section 18, ADC1 run the STM32H5 model and more"
    )
    assert group.body.startswith("- First bullet")


@pytest.mark.skipif(shutil.which("clang-format") is None, reason="clang-format is not installed")
def test_a_long_heading_survives_clang_format(tmp_path: Path) -> None:
    heading = (
        "On the STM32L47x/L48x, RM0351 section 18, ADC1, ADC2 and ADC3 run the STM32H5 model, "
        "and the other instances keep theirs"
    )
    source = tmp_path / "long.cpp"
    source.write_text(block("long", "Long").replace('"Differs from RM0456"', f'"{heading}"'))
    (tmp_path / ".clang-format").write_text((ROOT / ".clang-format").read_text())
    subprocess.run(["clang-format", "-i", str(source)], check=True, cwd=tmp_path)
    assert f'"{heading}"' not in source.read_text()
    assert limits_docs.read_pages([tmp_path])["long"].groups[0].heading == heading


def test_the_lead_is_the_first_sentence_without_doxygen_markup() -> None:
    lead = limits_docs.lead_sentence(
        ["@brief Model of @p x, e.g. a timer, RM0456 9.", "        It does more.", "", "@param y"]
    )
    assert lead == "Model of x, e.g. a timer, RM0456 9."
    assert limits_docs.lead_sentence(["@param y"]) == ""
    assert limits_docs.lead_sentence(["@brief One sentence"]) == "One sentence."


def test_a_page_without_references_or_lead_renders_without_them(tmp_path: Path) -> None:
    text = "/**\n * @limits t T\n *\n * @group all rule\n * - Plain.\n */\nint x;\n"
    write(tmp_path, "t.cpp", text=text)
    page = limits_docs.read_pages([tmp_path])["t"]
    rendered = limits_docs.render_page(page, lambda path: path.name)
    assert "References" not in rendered
    assert rendered.startswith("# T\n\nSources: [t.cpp](t.cpp)\n\n")


def test_the_table_lists_pages_by_title(tmp_path: Path) -> None:
    write(tmp_path, "b.cpp", "b", "Bravo")
    write(tmp_path, "a.cpp", "a", "alpha")
    table = limits_docs.render_table(limits_docs.read_pages([tmp_path]))
    assert [line.split("|")[1].strip() for line in table.splitlines()[2:4]] == [
        "[alpha](a.md)",
        "[Bravo](b.md)",
    ]
