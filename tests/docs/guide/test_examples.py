import re
from pathlib import Path

ROOT = Path(__file__).parents[3]


def test_every_example_readme_is_in_the_examples_page() -> None:
    page = (ROOT / "docs/guide/examples.md").read_text()
    included = set(re.findall(r'--8<-- "stm32u575/(\w+)/README\.md:\d+:\d+"', page))
    examples = {p.parent.name for p in (ROOT / "examples/stm32u575").glob("*/README.md")}
    assert included == examples
