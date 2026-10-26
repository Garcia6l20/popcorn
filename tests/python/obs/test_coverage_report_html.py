"""popcorn.coverage.report built-in html: files, escaping, classes and the path model."""

from __future__ import annotations

import os
import re
import time
from pathlib import Path

import pytest
from popcorn.coverage.report import parse_lcov, render_html

SOURCE = "int f(void) {\n  if (a < b && c) return 1;\n  return 2;\n}\n// <script>x</script>\n"

TRACEFILE = """TN:
SF:src/a.c
FN:1,4,f<g>
FNDA:2,f<g>
DA:1,2
DA:2,2
DA:3,0
BRDA:2,0,0,1
BRDA:2,0,1,-
BRF:2
BRH:1
end_of_record
TN:
SF:../x.c
DA:1,1
end_of_record
"""


def build(tmp_path: Path, text: str = TRACEFILE) -> tuple[Path, Path]:
    root = tmp_path / "root"
    (root / "src").mkdir(parents=True)
    (root / "src" / "a.c").write_text(SOURCE)
    out = tmp_path / "out"
    render_html(parse_lcov(text), out, root=root)
    return root, out


def test_pages_are_flat_and_named_by_position(tmp_path: Path) -> None:
    _, out = build(tmp_path)
    assert sorted(path.name for path in out.iterdir()) == [
        ".popcorn-html-report",
        "index.html",
        "src-0.html",
        "src-1.html",
        "style.css",
    ]
    assert not (tmp_path / "x.c").exists()


def test_source_text_and_names_are_escaped(tmp_path: Path) -> None:
    _, out = build(tmp_path)
    page = (out / "src-1.html").read_text()
    assert "a &lt; b &amp;&amp; c" in page
    assert "&lt;script&gt;x&lt;/script&gt;" in page
    assert "<script>" not in page
    assert "f&lt;g&gt;" in page
    assert "f<g>" not in page


def test_lines_are_classified_hit_miss_partial_none(tmp_path: Path) -> None:
    _, out = build(tmp_path)
    page = (out / "src-1.html").read_text()
    assert '<tr class="hit"><td class="no">1</td><td class="hits">2</td>' in page
    assert '<tr class="partial"><td class="no">2</td>' in page
    assert '<tr class="miss"><td class="no">3</td><td class="hits">0</td>' in page
    assert '<tr class="none"><td class="no">4</td><td class="hits"></td>' in page


def test_index_holds_totals_and_rates_sorted_by_display_name(tmp_path: Path) -> None:
    _, out = build(tmp_path)
    index = (out / "index.html").read_text()
    assert index.index('href="src-0.html">../x.c') < index.index('href="src-1.html">src/a.c')
    assert "66.66% (2/3)" in index
    assert "100.00% (1/1)" in index
    assert "50.00% (1/2)" in index


def test_a_relative_source_is_read_from_the_root_not_the_cwd(tmp_path: Path) -> None:
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    (elsewhere / "src").mkdir()
    (elsewhere / "src" / "a.c").write_text("WRONG\n")
    previous = Path.cwd()
    os.chdir(elsewhere)
    try:
        _, out = build(tmp_path)
    finally:
        os.chdir(previous)
    page = (out / "src-1.html").read_text()
    assert "WRONG" not in page
    assert "int f(void)" in page


def test_an_absolute_source_is_read_as_written(tmp_path: Path) -> None:
    source = tmp_path / "abs.c"
    source.write_text("int absolute;\n")
    out = tmp_path / "out"
    render_html(parse_lcov(f"SF:{source}\nDA:1,1\nend_of_record\n"), out, root=tmp_path / "other")
    assert "int absolute;" in (out / "src-0.html").read_text()


def test_an_unreadable_source_shows_counts_and_a_notice(tmp_path: Path) -> None:
    out = tmp_path / "out"
    render_html(parse_lcov("SF:missing.c\nDA:7,3\nend_of_record\n"), out, root=tmp_path)
    page = (out / "src-0.html").read_text()
    assert "could not be read" in page
    assert '<td class="no">7</td><td class="hits">3</td>' in page


def test_output_has_no_scripts_and_no_external_references(tmp_path: Path) -> None:
    _, out = build(tmp_path)
    for path in out.iterdir():
        text = path.read_text()
        assert "http" not in text
        assert "<script" not in text.replace("&lt;script", "")


def test_an_empty_report_renders_an_index(tmp_path: Path) -> None:
    out = tmp_path / "out"
    render_html(parse_lcov(""), out, root=tmp_path)
    assert "TOTAL" in (out / "index.html").read_text()


def rows(page: str) -> list[tuple[str, str]]:
    found = re.findall(
        r'<td class="no">(\d+)</td><td class="hits">\d*</td><td class="src">(.*?)</td>', page
    )
    return [(number, text) for number, text in found]


def test_sources_split_on_newlines_only(tmp_path: Path) -> None:
    root = tmp_path / "root"
    root.mkdir()
    (root / "a.c").write_bytes(
        b"int a;\n\x0cint b;\xe2\x80\xa8int c;\xc2\x85\nint d;\r\nint e;\r\n"
    )
    out = tmp_path / "out"
    render_html(
        parse_lcov("SF:a.c\nDA:1,1\nDA:2,0\nDA:3,1\nDA:4,1\nend_of_record\n"), out, root=root
    )
    found = rows((out / "src-0.html").read_text())
    assert [number for number, _ in found] == ["1", "2", "3", "4"]
    assert found[1][1] == "\x0cint b;\u2028int c;\x85"
    assert found[2][1] == "int d;"
    assert found[3][1] == "int e;"


def test_a_huge_counted_line_does_not_grow_the_page(tmp_path: Path) -> None:
    root = tmp_path / "root"
    root.mkdir()
    (root / "a.c").write_text("a\nb\nc\n")
    out = tmp_path / "out"
    start = time.monotonic()
    render_html(parse_lcov("SF:a.c\nDA:1,1\nDA:2000000000,1\nend_of_record\n"), out, root=root)
    assert time.monotonic() - start < 1
    assert [number for number, _ in rows((out / "src-0.html").read_text())] == [
        "1",
        "2",
        "3",
        "2000000000",
    ]


def test_a_source_that_is_not_a_regular_file_or_too_large_shows_the_notice(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    root = tmp_path / "root"
    root.mkdir()
    os.mkfifo(root / "pipe.c")
    (root / "big.c").write_text("x\n" * 10)
    monkeypatch.setattr("popcorn.coverage.report._html.SOURCE_LIMIT", 5)
    out = tmp_path / "out"
    text = "SF:big.c\nDA:1,1\nend_of_record\nSF:pipe.c\nDA:2,1\nend_of_record\n"
    render_html(parse_lcov(text), out, root=root)
    for name in ("src-0.html", "src-1.html"):
        page = (out / name).read_text()
        assert "could not be read" in page
        assert "<td>x</td>" not in page
