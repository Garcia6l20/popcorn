import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "1b3acbd" in popcorn.version()
