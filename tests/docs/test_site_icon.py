import tomllib
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

ROOT = Path(__file__).parents[2]
THEME = tomllib.loads((ROOT / "zensical.toml").read_text())["project"]["theme"]


@pytest.mark.parametrize("key", ["logo", "favicon"])
def test_the_theme_points_to_an_svg_file(key: str) -> None:
    path = ROOT / "docs" / THEME[key]
    assert ET.parse(path).getroot().tag == "{http://www.w3.org/2000/svg}svg"


@pytest.mark.skipif(not (ROOT / "site/index.html").exists(), reason="the site is not built")
def test_the_built_page_shows_the_logo_and_links_the_favicon() -> None:
    page = (ROOT / "site/index.html").read_text()
    assert f'<link rel="icon" href="./{THEME["favicon"]}">' in page
    assert f'src="./{THEME["logo"]}"' in page
