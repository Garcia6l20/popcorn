import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "29b3de1" in popcorn.version()
