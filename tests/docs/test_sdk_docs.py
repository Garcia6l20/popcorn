import re
import shutil
from pathlib import Path

import pytest

from popcorn_build import docs
from popcorn_build.sdk import sdk_inputs

ROOT = Path(__file__).parents[2]
INCLUDE = ROOT / "native/include"


def doxygen_inputs() -> list[Path]:
    text = docs.DOXYFILE.read_text().replace("\\\n", " ")
    match = re.search(r"^INPUT\s*=(.*)$", text, re.MULTILINE)
    assert match is not None
    return [ROOT / name for name in match.group(1).split() if not name.endswith(".md")]


def test_the_doxygen_inputs_are_public_headers_of_the_sdk() -> None:
    headers = set(sdk_inputs(INCLUDE, []))
    for path in doxygen_inputs():
        assert path.exists(), path
        found = list(path.rglob("*.hpp")) if path.is_dir() else [path]
        assert found
        assert set(found) <= headers


def test_without_doxygen_the_build_warns_once_and_writes_a_stub(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:

    def missing(_name: str) -> None:
        return None

    monkeypatch.setattr(shutil, "which", missing)
    assert docs.build_sdk_reference(tmp_path / "api") == 0
    assert capsys.readouterr().err.count("doxygen not found") == 1
    assert "doxygen" in (tmp_path / "api/index.html").read_text()


@pytest.mark.skipif(shutil.which("doxygen") is None, reason="doxygen is not installed")
def test_doxygen_reports_no_undocumented_public_declaration(tmp_path: Path) -> None:
    assert docs.build_sdk_reference(tmp_path / "api") == 0
    pages = {page.name for page in (tmp_path / "api").glob("*.html")}
    assert "index.html" in pages
    assert any(name.startswith("classpopcorn_1_1periph_1_1model") for name in pages)


def rendered(directory: Path) -> str:
    text = ""
    for page in directory.glob("*.html"):
        if not page.name.endswith("_source.html"):
            text += re.sub(r"<[^>]+>", "", page.read_text()).replace("&#160;", " ")
    return re.sub(r"\s+", " ", text)


@pytest.mark.skipif(shutil.which("doxygen") is None, reason="doxygen is not installed")
def test_namespace_scope_declarations_are_rendered(tmp_path: Path) -> None:
    assert docs.build_sdk_reference(tmp_path / "api") == 0
    text = rendered(tmp_path / "api")
    assert "enum class popcorn::stop_reason" in text
    assert "glob_match(" in text
    assert "vtime_from_instructions(" in text
    assert "fits_access(" in text
