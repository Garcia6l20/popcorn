import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "f78d3a5" in popcorn.version()
