"""popcorn.coverage.report Cobertura xml: shape, path model and rates of `render_cobertura`."""

from __future__ import annotations

import time
from pathlib import Path
from xml.etree import ElementTree

import pytest
from popcorn.coverage.report import generate, parse_lcov, render_cobertura
from popcorn.coverage.report._spec import ReportSpec

ROOT = Path("/work/proj")
TIMESTAMP = 1_700_000_000_123

TRACEFILE = """TN:
SF:/work/proj/src/driver.c
FN:1,4,add
FN:6,9,sub
FNDA:3,add
FNDA:0,sub
DA:1,3
DA:2,3
DA:7,0
DA:8,0
BRDA:2,0,0,1
BRDA:2,0,1,-
end_of_record
TN:
SF:/opt/sdk/lib/a&b.c
DA:1,5
end_of_record
TN:
SF:tests/firmware/shared_exit.S
FN:3,5,start
FNDA:1,start
DA:3,1
DA:4,1
DA:5,1
end_of_record
"""

GOLDEN = (Path(__file__).parents[2] / "data" / "obs" / "cobertura_golden.xml").read_text()


def render(text: str = TRACEFILE, root: Path = ROOT) -> str:
    return render_cobertura(parse_lcov(text), root=root, timestamp=TIMESTAMP)


def parsed(text: str = TRACEFILE, root: Path = ROOT) -> ElementTree.Element:
    return ElementTree.fromstring(render(text, root))


def classes(top: ElementTree.Element) -> dict[str, ElementTree.Element]:
    return {
        element.get("filename", ""): element
        for element in top.iterfind("./packages/package/classes/class")
    }


def test_golden_file_with_branch_absolute_inside_outside_and_relative_paths() -> None:
    version = parsed().get("version")
    assert render() == GOLDEN.replace("VERSION", str(version))


def test_the_root_rates_equal_the_totals() -> None:
    top = parsed()
    totals = parse_lcov(TRACEFILE).totals()
    assert float(top.get("line-rate", "")) == totals.lines.hit / totals.lines.found
    assert float(top.get("branch-rate", "")) == totals.branches.hit / totals.branches.found
    assert top.get("lines-valid") == str(totals.lines.found)
    assert top.get("lines-covered") == str(totals.lines.hit)
    assert top.get("branches-valid") == str(totals.branches.found)
    assert top.get("branches-covered") == str(totals.branches.hit)


def test_a_relative_sf_is_its_own_filename_in_its_directory_package() -> None:
    top = parsed()
    relative = classes(top)["tests/firmware/shared_exit.S"]
    assert relative.get("name") == "shared_exit.S"
    packages = [element.get("name") for element in top.iterfind("./packages/package")]
    assert "tests/firmware" in packages


def test_a_relative_sf_stays_as_written_under_any_root() -> None:
    top = parsed("TN:\nSF:tests/a.c\nDA:1,1\nend_of_record\n", Path("/elsewhere"))
    assert list(classes(top)) == ["tests/a.c"]
    assert [element.text for element in top.iterfind("./sources/source")] == ["/elsewhere"]


def test_an_absolute_sf_under_root_is_relative_and_outside_keeps_its_path_and_lists_slash() -> None:
    top = parsed()
    assert set(classes(top)) == {
        "src/driver.c",
        "/opt/sdk/lib/a&b.c",
        "tests/firmware/shared_exit.S",
    }
    assert [element.text for element in top.iterfind("./sources/source")] == ["/work/proj", "/"]


def test_slash_is_not_a_source_when_every_path_is_under_root() -> None:
    top = parsed("TN:\nSF:/work/proj/a.c\nDA:1,1\nend_of_record\n")
    assert [element.text for element in top.iterfind("./sources/source")] == ["/work/proj"]


def test_packages_wrap_the_package_elements() -> None:
    top = parsed()
    assert [child.tag for child in top] == ["sources", "packages"]
    packages = top.find("packages")
    assert packages is not None
    assert all(child.tag == "package" for child in packages)


def test_a_name_with_an_ampersand_is_escaped_and_reads_back() -> None:
    text = render()
    assert 'name="a&amp;b.c"' in text
    assert "a&b" not in text
    assert "/opt/sdk/lib/a&b.c" in classes(ElementTree.fromstring(text))


def test_the_timestamp_is_the_given_milliseconds() -> None:
    assert parsed().get("timestamp") == str(TIMESTAMP)


def test_package_rates_sum_their_files() -> None:
    text = (
        "TN:\nSF:/work/proj/s/a.c\nDA:1,1\nDA:2,0\nend_of_record\n"
        "TN:\nSF:/work/proj/s/b.c\nDA:1,1\nDA:2,1\nDA:3,1\nDA:4,1\nend_of_record\n"
    )
    (package,) = parsed(text).iterfind("./packages/package")
    assert package.get("name") == "s"
    assert float(package.get("line-rate", "")) == 5 / 6


def test_an_empty_report_is_a_valid_document() -> None:
    top = parsed("")
    assert top.get("lines-valid") == "0"
    assert top.get("line-rate") == "1.0"
    assert list(top.iterfind("./packages/package")) == []


def test_a_branch_line_condition_cuts_the_percent() -> None:
    text = (
        "TN:\nSF:/work/proj/a.c\nDA:1,1\nBRDA:1,0,0,1\nBRDA:1,0,1,1\nBRDA:1,0,2,0\nend_of_record\n"
    )
    (line,) = parsed(text).iterfind(".//class/lines/line")
    assert line.get("condition-coverage") == "66% (2/3)"
    assert line.get("branch") == "true"


def test_generate_writes_the_xml_atomically_with_the_given_timestamp(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "deep" / "c.xml"
    result = generate(source, [ReportSpec("xml", out)], root=ROOT, timestamp=TIMESTAMP)
    assert result.written == (out,)
    assert out.read_text() == render()
    assert [path.name for path in out.parent.iterdir()] == ["c.xml"]


def test_generate_filters_the_xml(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "c.xml"
    generate(source, [ReportSpec("xml", out)], omit=["**/driver.c"], root=ROOT, timestamp=1)
    assert "src/driver.c" not in set(classes(ElementTree.parse(out).getroot()))


def test_generate_without_a_timestamp_writes_the_current_milliseconds(tmp_path: Path) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "c.xml"
    before = int(time.time() * 1000)
    generate(source, [ReportSpec("xml", out)], root=ROOT)
    stamp = int(ElementTree.parse(out).getroot().get("timestamp", "0"))
    assert before <= stamp <= int(time.time() * 1000)


def test_a_failed_rename_keeps_the_old_file_and_leaves_no_temporary(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    source = tmp_path / "in.info"
    source.write_text(TRACEFILE)
    out = tmp_path / "c.xml"
    out.write_text("old")

    def refuse(self: Path, target: object) -> Path:
        raise OSError("no rename")

    monkeypatch.setattr(Path, "replace", refuse)
    with pytest.raises(OSError, match="no rename"):
        generate(source, [ReportSpec("xml", out)], root=ROOT, timestamp=1)
    assert out.read_text() == "old"
    assert sorted(path.name for path in tmp_path.iterdir()) == ["c.xml", "in.info"]


def test_a_branch_line_with_no_da_counts_in_the_rates_and_has_no_line_element() -> None:
    text = "TN:\nSF:/work/proj/a.c\nBRDA:5,0,0,1\nend_of_record\n"
    top = parsed(text)
    assert top.get("branches-valid") == "1"
    assert top.get("branches-covered") == "1"
    assert [line.get("number") for line in top.iterfind(".//class/lines/line")] == []


def test_a_tiny_rate_is_never_in_exponent_form() -> None:
    text = "TN:\nSF:/work/proj/a.c\nDA:1,1\n" + "".join(
        f"DA:{number},0\n" for number in range(2, 100_001)
    )
    top = parsed(text + "end_of_record\n")
    assert top.get("line-rate") == "0.00001"
    assert all("e" not in (element.get("line-rate") or "") for element in top.iter())


def test_control_characters_in_names_are_dropped_and_the_xml_parses() -> None:
    text = "TN:\nSF:/work/proj/a\x01b\x1f.c\nFN:1,2,f\x00n\nFNDA:1,f\x00n\nDA:1,1\nend_of_record\n"
    top = parsed(text)
    assert list(classes(top)) == ["ab.c"]
    (method,) = top.iterfind(".//method")
    assert method.get("name") == "fn"


REQUIRED = {
    "coverage": {
        "line-rate",
        "branch-rate",
        "lines-covered",
        "lines-valid",
        "branches-covered",
        "branches-valid",
        "complexity",
        "version",
        "timestamp",
    },
    "package": {"name", "line-rate", "branch-rate", "complexity"},
    "class": {"name", "filename", "line-rate", "branch-rate", "complexity"},
    "method": {"name", "signature", "line-rate", "branch-rate"},
    "line": {"number", "hits"},
}


def test_every_element_carries_the_attributes_the_dtd_requires() -> None:
    seen: set[str] = set()
    for element in parsed().iter():
        if element.tag in REQUIRED:
            seen.add(element.tag)
            assert REQUIRED[element.tag] <= set(element.keys()), element.tag
    assert seen == set(REQUIRED)
