import re
from pathlib import Path

import popcorn
import popcorn.plugin
import popcorn.pytest

PAGES = Path(__file__).parents[2] / "docs/reference/python"


def referenced() -> set[str]:
    text = "".join(page.read_text() for page in PAGES.glob("*.md"))
    return set(re.findall(r"^::: (\S+)$", text, re.MULTILINE))


def test_every_public_name_is_on_a_reference_page() -> None:
    names = {f"popcorn.{name}" for name in popcorn.__all__}
    names |= {f"popcorn.pytest.{name}" for name in popcorn.pytest.__all__}
    names |= {f"popcorn.plugin.{name}" for name in popcorn.plugin.__all__}
    assert names <= referenced()


def test_no_page_documents_a_name_that_does_not_exist() -> None:
    public = {f"popcorn.{name}" for name in popcorn.__all__}
    public |= {f"popcorn.pytest.{name}" for name in popcorn.pytest.__all__}
    public |= {f"popcorn.plugin.{name}" for name in popcorn.plugin.__all__}
    assert referenced() <= public
