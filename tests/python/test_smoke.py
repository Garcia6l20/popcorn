import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "7a9c1a3" in popcorn.version()
