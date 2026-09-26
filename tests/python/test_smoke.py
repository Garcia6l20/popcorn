import popcorn


def test_version_names_unicorn_commit() -> None:
    assert "51fe058" in popcorn.version()
