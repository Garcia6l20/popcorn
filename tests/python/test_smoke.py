import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "b3fce5f" in popcorn.version()
