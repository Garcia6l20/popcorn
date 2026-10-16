import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "f3ec5cd" in popcorn.version()
