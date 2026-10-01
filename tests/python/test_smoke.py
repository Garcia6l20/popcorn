import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "c388e25" in popcorn.version()
