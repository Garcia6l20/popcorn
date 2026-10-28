import re
from pathlib import Path

import pytest

ROOT = Path(__file__).parents[3]
CHIPS = sorted(p.name for p in (ROOT / "examples").iterdir() if p.is_dir())


@pytest.mark.parametrize("chip", CHIPS)
def test_every_example_readme_is_in_the_examples_page(chip: str) -> None:
    page = (ROOT / "docs/guide/examples.md").read_text()
    included = set(re.findall(rf'--8<-- "{chip}/(\w+)/README\.md:\d+:\d+"', page))
    examples = {p.parent.name for p in (ROOT / "examples" / chip).glob("*/README.md")}
    assert included == examples


def test_every_example_test_module_has_its_own_basename() -> None:
    names = [p.name for p in (ROOT / "examples").glob("*/*/test_*.py")]
    assert sorted(names) == sorted(set(names))
