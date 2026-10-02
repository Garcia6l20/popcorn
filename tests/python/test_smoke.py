import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "a8dc2c6" in popcorn.version()
