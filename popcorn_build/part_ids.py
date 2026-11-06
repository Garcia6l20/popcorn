"""A pytest plugin that writes the tests that ran on the `--popcorn-part` part, per file.

`-p popcorn_build.part_ids --part-ids-file FILE` writes, at the end of the session, a JSON object
mapping the absolute path of each test file to the number of its tests that built a machine on the
part and did not skip, from `popcorn.pytest._session.part_tests`.
"""

from __future__ import annotations

import json
from collections import Counter
from collections.abc import Iterable
from pathlib import Path

import pytest
from popcorn.pytest._session import part_tests


def file_counts(nodes: Iterable[str], root: Path) -> dict[str, int]:
    """The number of @p nodes per test file, the files as absolute paths under @p root."""
    return dict(Counter(str((root / node.split("::")[0]).resolve()) for node in nodes))


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--part-ids-file", default=None, help="Where to write the part test files.")


@pytest.hookimpl(trylast=True)
def pytest_sessionfinish(session: pytest.Session) -> None:
    config = session.config
    target = config.getoption("part_ids_file")
    if target is None or hasattr(config, "workerinput"):
        return
    Path(target).write_text(json.dumps(file_counts(part_tests(config), config.rootpath)))
