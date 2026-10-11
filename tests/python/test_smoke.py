import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "04ecd22" in popcorn.version()
