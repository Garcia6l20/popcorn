import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "c66672d" in popcorn.version()
