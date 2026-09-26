import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "2a9d2c6" in popcorn.version()
