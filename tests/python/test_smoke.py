import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "b5ed2be" in popcorn.version()
