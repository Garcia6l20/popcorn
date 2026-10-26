"""popcorn.coverage.report specs: `kind[:target]` parsing and the lcov report of `generate`."""

from __future__ import annotations

from pathlib import Path

import pytest
from popcorn.coverage.report import ReportSpec, generate, parse_lcov, parse_spec

PREBUILT = Path(__file__).parents[2] / "firmware" / "obs" / "prebuilt"
TRACEFILE = (
    "TN:\nSF:src/a.c\nFN:1,3,f\nFNDA:2,f\nFNF:1\nFNH:1\nDA:1,2\nDA:2,0\nLF:2\nLH:1\n"
    "end_of_record\nTN:\nSF:src/b.c\nDA:1,1\nLF:1\nLH:1\nend_of_record\n"
)


def test_parse_spec_defaults_and_targets() -> None:
    base = Path("/base")
    assert parse_spec("term") == ReportSpec("term")
    assert parse_spec("html") == ReportSpec("html", Path("htmlcov"))
    assert parse_spec("xml", base) == ReportSpec("xml", base / "coverage.xml")
    assert parse_spec("lcov") == ReportSpec("lcov")
    assert parse_spec("lcov:out/a.info", base) == ReportSpec("lcov", base / "out" / "a.info")
    assert parse_spec("html:/abs/dir", base) == ReportSpec("html", Path("/abs/dir"))


@pytest.mark.parametrize("text", ["json", "term:x", "xml:", "html:", "lcov:", ":x", ""])
def test_parse_spec_refuses(text: str) -> None:
    with pytest.raises(ValueError):
        parse_spec(text)


def test_generate_lcov_without_filter_keeps_the_bytes(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "deep" / "out.info"
    result = generate(source, [ReportSpec("lcov", out)], root=tmp_path)
    assert out.read_text() == TRACEFILE
    assert result.written == (out,)
    assert result.totals.lines.found == 3


def test_generate_lcov_filters_and_counts_the_kept_files(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out.info"
    result = generate(source, [ReportSpec("lcov", out)], omit=["**/b.c"], root=tmp_path)
    kept = parse_lcov(out.read_text())
    assert [file.path for file in kept.files] == ["src/a.c"]
    assert result.totals.lines.found == 2


def test_generate_refuses_what_it_cannot_write(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    with pytest.raises(ValueError):
        generate(source, [ReportSpec("lcov")], root=tmp_path)
    for kind in ("html", "xml"):
        with pytest.raises(NotImplementedError):
            generate(source, [ReportSpec(kind, tmp_path / kind)], root=tmp_path)


def test_generate_returns_the_term_text_and_writes_nothing(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    result = generate(source, [ReportSpec("term")], omit=["**/b.c"], root=tmp_path)
    assert result.written == ()
    assert result.term is not None
    assert "src/a.c" in result.term
    assert "src/b.c" not in result.term
    assert generate(source, [], root=tmp_path).term is None


def test_generate_checks_every_spec_before_writing(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "out.info"
    with pytest.raises(NotImplementedError):
        generate(
            source, [ReportSpec("lcov", out), ReportSpec("html", tmp_path / "c")], root=tmp_path
        )
    with pytest.raises(ValueError):
        generate(source, [ReportSpec("lcov", out), ReportSpec("lcov")], root=tmp_path)
    assert not out.exists()


def test_generate_copies_bytes_whatever_the_locale(tmp_path: Path) -> None:
    text = TRACEFILE.replace("src/a.c", "src/\u00e9.c")
    source = tmp_path / "in.info"
    raw = text.replace("\n", "\r\n").encode()
    source.write_bytes(raw)
    out = tmp_path / "out.info"
    generate(source, [ReportSpec("lcov", out)], root=tmp_path)
    assert out.read_bytes() == raw
    generate(source, [ReportSpec("lcov", out)], omit=["**/b.c"], root=tmp_path)
    assert "src/\u00e9.c" in out.read_bytes().decode()
