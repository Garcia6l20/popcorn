import re
from pathlib import Path

import pytest

ROOT = Path(__file__).parents[2]
PAGES = sorted((ROOT / "docs/dev").glob("*.md"))
PATH = re.compile(
    r"`([A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)+\.(?:cpp|hpp|py|toml|json|md|map|sed|supp))`"
)
BASES = (
    ROOT,
    ROOT / "native/include",
    ROOT / "native/src/core",
    ROOT / "native/src/image",
    ROOT / "native/src/bind",
    ROOT / "tests/native",
    ROOT / "python",
    ROOT / "python/popcorn/stm32/description",
)


def mentioned() -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    for page in PAGES:
        found += [(page.name, path) for path in PATH.findall(page.read_text())]
    return found


@pytest.mark.parametrize(("page", "path"), mentioned())
def test_a_path_a_dev_page_names_exists(page: str, path: str) -> None:
    assert any((base / path).exists() for base in BASES), f"{page} names {path}"
