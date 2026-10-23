import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "f8d7eae" in popcorn.version()
