import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "ff7797e" in popcorn.version()
