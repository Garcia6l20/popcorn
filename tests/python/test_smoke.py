import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "182707e" in popcorn.version()
