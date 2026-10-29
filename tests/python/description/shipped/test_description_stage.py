"""The package imported by the tests holds the data committed under `python/`."""

from pathlib import Path

import popcorn.stm32.description as description
import pytest
from popcorn.stm32.description import _shipped

SOURCE = Path(__file__).parents[4] / "python" / "popcorn" / "stm32"
STAGED = Path(description.__file__).parents[1]


def _files(root: Path) -> dict[str, bytes]:
    found = {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in (root / "data").glob("*.json.xz")
    }
    found.update(
        {
            path.relative_to(root).as_posix(): path.read_bytes()
            for path in (root / "description" / "patches").glob("*.toml")
        }
    )
    found.update(
        {
            path.relative_to(root).as_posix(): path.read_bytes()
            for path in (root / "description" / "rzone").glob("*.toml")
        }
    )
    return found


def test_the_staged_shipped_data_patches_and_facts_equal_the_committed_files() -> None:
    if STAGED.resolve() == SOURCE.resolve():
        pytest.skip("the package is imported from the source tree")
    assert _shipped.SHIPPED_DIR == STAGED / "data"
    staged = _files(STAGED)
    committed = _files(SOURCE)
    assert sorted(staged) == sorted(committed), "build the package: the staged file set differs"
    assert [
        name for name in staged if staged[name] != committed[name]
    ] == [], "build the package: staged files differ from python/popcorn/stm32"
