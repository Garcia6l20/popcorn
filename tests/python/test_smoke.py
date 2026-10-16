import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "57ad47c" in popcorn.version()
